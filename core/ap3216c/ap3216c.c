// SPDX-License-Identifier: GPL-2.0
/*
 * 文件：ap3216c.c
 * 所属模块：core/ap3216c — P7-4 AP3216C 环境光/接近/红外三合一传感器驱动
 * 作用：把 AP3216C 这颗 I2C 传感器的数据暴露给用户态，供上层做自动背光、接近检测等
 * 核心功能：
 *   - 通过 I2C 读写芯片寄存器，支持 ALS(环境光 16bit)、PS(接近 10bit)、IR(红外 10bit)
 *   - ALS 支持 4 档量程切换，对应不同的 lux 分辨率
 *   - 探测溢出标志并做归零处理，避免读到错误的大数值
 * 对外接口（用户态怎么用）：
 *   - 无 /dev 节点，走 IIO 子系统 sysfs：
 *     /sys/bus/iio/devices/iio:deviceX/in_illuminance_both_raw      # ALS 原始值
 *     /sys/bus/iio/devices/iio:deviceX/in_illuminance_both_scale    # ALS 量程(写此节点切换量程)
 *     /sys/bus/iio/devices/iio:deviceX/in_proximity_raw             # PS 原始值
 *     /sys/bus/iio/devices/iio:deviceX/in_illuminance_ir_raw        # IR 原始值
 *   - 例：cat in_illuminance_both_raw；echo 0.0078 > in_illuminance_both_scale
 * 依赖内核子系统：
 *   - i2c_driver（I2C 总线驱动框架，匹配设备树 compatible）
 *   - regmap（I2C 寄存器抽象，8bit 寄存器/8bit 值，逐字节读）
 *   - iio_dev + IIO sysfs（工业 I/O 子系统，提供通道与 scale 机制）
 *   - mutex（并发保护，避免多进程同时读写寄存器错乱）
 *
 * 板级经验（沿用 P4-B misc 版，勿丢）:
 *   - 逐字节读寄存器：曾用连续 bulk 读 2/6 字节，板端实测数据帧交替失效
 *     (IR_OF/PS_OF 置位 → 全 0 帧)，故 16bit 数据也拆两次单字节读；
 *   - PS 寄存器 0x0E/0x0F（手册骨架 0x10 系笔误），值 = (hi&0x3F)<<4 | (lo&0x0F)；
 *   - 溢出标志（IR: hi.bit7 / PS: hi.bit6）置位时对应值归 0；
 *   - probe 一次初始化（软复位 → 使能 ALS+PS+IR），不重复复位避免数据流中断。
 */
#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/delay.h>
#include <linux/mutex.h>
#include <linux/regmap.h>
#include <linux/iio/iio.h>
#include <linux/iio/sysfs.h>

/* 设备名：同时用于 i2c_driver.name 与 iio_dev.name */
#define AP3216C_NAME "ap3216c"

/* AP3216C 寄存器地址表（参考正点原子 ap3216creg.h） */
#define AP3216C_SYS_CONF	0x00	/* 系统配置寄存器：写 0x04 软复位，写 0x03 使能 ALS+PS+IR */
#define AP3216C_IR_DATA_L	0x0A	/* IR 数据低字节（IR 占用 0x0A/0x0B，10bit 有效） */
#define AP3216C_ALS_DATA_L	0x0C	/* ALS 数据低字节（ALS 占用 0x0C/0x0D，16bit 有效） */
#define AP3216C_PS_DATA_L	0x0E	/* PS 数据低字节（PS 占用 0x0E/0x0F，10bit 有效，手册 0x10 为笔误） */
#define AP3216C_ALS_CONF	0x10	/* ALS 配置寄存器：bit[5:4] 选择量程档位 */

/* 系统配置写入值 */
#define AP3216C_CONF_RESET	0x04	/* 软复位命令：芯片复位后需延时等待就绪 */
#define AP3216C_CONF_ENABLE	0x03	/* 使能命令：bit0=ALS, bit1=PS+IR 同时开启 */

/* 扫描元素索引：对应 iio_chan_spec.scan_index，虽本驱动为 sysfs 直读模式，
 * 但 IIO 框架仍要求每个通道有唯一索引，用于 buffered 模式预留 */
enum ap3216c_scan {
	AP3216C_ALS,	/* 环境光通道 */
	AP3216C_PS,	/* 接近通道 */
	AP3216C_IR,	/* 红外通道 */
};

/* ALS 量程表：单位 lux×1e6（即微 lux），对应 ALSCONFIG[5:4]=0..3
 * 四档物理量程依次 0~20661 / 0~5162 / 0~1291 / 0~323 lux，
 * 数值越小代表分辨率越细（适合暗光），数值越大代表量程越大 */
static const int als_scale_tbl[] = { 315000, 78800, 19700, 4900 };

/* 设备私有数据：每个 I2C 从机对应一个实例，挂在 iio_dev 的私有区 */
struct ap3216c_dev {
	struct i2c_client *client;	/* 指向 I2C 客户端，用于打印与取 dev */
	struct regmap *regmap;		/* 寄存器映射：封装 I2C 读写细节 */
	struct mutex lock;		/* 互斥锁：保护 regmap 读写与量程切换的原子性 */
};

/* IIO 通道表：向 IIO 子系统声明本设备有哪些数据通道
 * 每个元素对应一个 sysfs 文件；ALS 带 SCALE 属性，其余只有 RAW */
static const struct iio_chan_spec ap3216c_channels[] = {
	{	/* ALS 环境光（16bit，可设量程） */
		.type = IIO_INTENSITY,
		.modified = 1,
		.channel2 = IIO_MOD_LIGHT_BOTH,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |
				      BIT(IIO_CHAN_INFO_SCALE),
		.scan_index = AP3216C_ALS,
	},
	{	/* PS 接近（10bit） */
		.type = IIO_PROXIMITY,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),
		.scan_index = AP3216C_PS,
	},
	{	/* IR 红外（10bit） */
		.type = IIO_INTENSITY,
		.modified = 1,
		.channel2 = IIO_MOD_LIGHT_IR,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),
		.scan_index = AP3216C_IR,
	},
};

/*
 * 功能：读 16bit 传感器数据（通用 helper）
 * 说明：明明是 16bit 却故意拆两次单字节读——板端实测若用 bulk 连续读，
 *       会出现溢出标志位错位导致全 0 帧，故用最稳妥的单字节方式
 * 参数：d - 设备私有结构；reg_low - 低字节寄存器地址；val - 输出 16bit 原始值 (hi<<8|lo)
 * 返回：0 成功，负值表示 I2C 传输失败
 * 调用方：ap3216c_read_raw() 的各个分支
 */
static int ap3216c_read_data16(struct ap3216c_dev *d, u8 reg_low,
			       unsigned int *val)
{
	unsigned int lo, hi;
	int ret;

	ret = regmap_read(d->regmap, reg_low, &lo);
	if (ret)
		return ret;
	ret = regmap_read(d->regmap, reg_low + 1, &hi);
	if (ret)
		return ret;

	*val = (hi << 8) | lo;
	return 0;
}

/*
 * 功能：芯片上电初始化
 * 流程：软复位(0x00←0x04) → 延时 50ms 等芯片就绪 → 使能 ALS+PS+IR(0x00←0x03) → ALS 量程设为 0 档
 * 参数：d - 设备私有结构（regmap 已就绪）
 * 返回：0 始终成功（寄存器写失败暂不视为致命，由上层重试或后续读失败体现）
 * 调用方：ap3216c_probe()，仅在 probe 时执行一次，避免运行时重复复位打断数据流
 */
static int ap3216c_init_chip(struct ap3216c_dev *d)
{
	regmap_write(d->regmap, AP3216C_SYS_CONF, AP3216C_CONF_RESET);
	msleep(50);		/* 复位最少 10ms，取 50ms 稳妥 */
	regmap_write(d->regmap, AP3216C_SYS_CONF, AP3216C_CONF_ENABLE);
	regmap_write(d->regmap, AP3216C_ALS_CONF, 0x00);	/* ALS 量程 0（最大） */
	return 0;
}

/*
 * 功能：IIO 框架回调——用户读 sysfs 节点时被调用
 * 参数：indio_dev - IIO 设备；chan - 指出读哪个通道(ALS/PS/IR)；
 *       val/val2 - 输出值（根据返回类型决定如何解读）；mask - 区分读 RAW 还是 SCALE
 * 返回：IIO_VAL_INT 表示 *val 有效；IIO_VAL_INT_PLUS_MICRO 表示 *val + *val2/1e6 有效；负值表错
 * 调用方：IIO 核心，当用户 cat /sys/.../in_*.raw 或 _scale 时触发
 * 关键逻辑：
 *   - RAW 分支加 mutex 保护寄存器读写；根据通道类型拆不同位拼接
 *   - IR/PS 需检查溢出位（bit15 的 0x8000/0x4000），溢出则归零
 *   - SCALE 分支读 ALS_CONF 寄存器高位查表得微 lux 值
 */
static int ap3216c_read_raw(struct iio_dev *indio_dev,
			    struct iio_chan_spec const *chan,
			    int *val, int *val2, long mask)
{
	struct ap3216c_dev *dev = iio_priv(indio_dev);
	unsigned int data, regdata;
	int ret = 0;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		mutex_lock(&dev->lock);	/* 保护：多进程并发 cat 时寄存器读写不交错 */
		switch (chan->type) {
		case IIO_INTENSITY:
			if (chan->channel2 == IIO_MOD_LIGHT_BOTH) {
				/* ALS：16bit 全有效，直接 hi<<8|lo */
				ret = ap3216c_read_data16(dev, AP3216C_ALS_DATA_L, &data);
				if (!ret)
					*val = (int)data;
			} else {
				/* IR：10bit 特殊拼接 + 溢出判断
				 * 硬件格式：hi[7]=IR_OF(溢出), hi[1:0]为高2位, lo[7:0]为低8位中的高8? 实际驱动按
				 * ((hi>>8)<<2)|(lo&0x03) 还原 10bit；若 IR_OF 置位说明光强超出量程，数据不可信归零 */
				ret = ap3216c_read_data16(dev, AP3216C_IR_DATA_L, &data);
				if (!ret) {
					if (data & 0x8000)
						data = 0;
					*val = (int)((data >> 8) << 2) | (int)(data & 0x03);
				}
			}
			break;
		case IIO_PROXIMITY:
			/* PS：10bit 特殊拼接 + 溢出判断
			 * 硬件格式：hi[6]=PS_OF, hi[5:0]为高6位, lo[3:0]为低4位，故 (hi&0x3F)<<4 | (lo&0x0F)；
			 * 溢出置位同样归零，避免误判为极近 */
			ret = ap3216c_read_data16(dev, AP3216C_PS_DATA_L, &data);
			if (!ret) {
				if (data & 0x4000)
					data = 0;
				*val = (int)(((data >> 8) & 0x3F) << 4) |
				       (int)(data & 0x0F);
			}
			break;
		default:
			ret = -EINVAL;
			break;
		}
		mutex_unlock(&dev->lock);
		return ret ? ret : IIO_VAL_INT;

	case IIO_CHAN_INFO_SCALE:	/* 读 ALS 量程：把寄存器值换算成用户可理解的 lux 分辨率 */
		if (chan->type != IIO_INTENSITY)
			return -EINVAL;
		mutex_lock(&dev->lock);
		/* 读 ALS_CONF，取 bit[5:4] 查表；表值单位微 lux，最终用户用 raw*scale 得 lux */
		ret = regmap_read(dev->regmap, AP3216C_ALS_CONF, &regdata);
		if (!ret) {
			*val = 0;
			*val2 = als_scale_tbl[(regdata & 0x30) >> 4];
		}
		mutex_unlock(&dev->lock);
		return ret ? ret : IIO_VAL_INT_PLUS_MICRO;

	default:
		return -EINVAL;
	}
}

/*
 * 功能：IIO 框架回调——用户写 sysfs scale 节点时被调用（切换 ALS 量程）
 * 参数：val/val2 - 用户写入的数值（本驱动只认 val2，需匹配 scale 表中某一档）；mask 必须为 SCALE
 * 返回：0 成功，-EINVAL 表示参数非法
 * 调用方：IIO 核心，当用户 echo xxx > in_illuminance_both_scale 时触发
 * 原理：遍历 scale 表找到相等项，把索引 i 写入 ALS_CONF[5:4]，即切换硬件量程
 */
static int ap3216c_write_raw(struct iio_dev *indio_dev,
			     struct iio_chan_spec const *chan,
			     int val, int val2, long mask)
{
	struct ap3216c_dev *dev = iio_priv(indio_dev);
	int i, ret = -EINVAL;

	if (mask != IIO_CHAN_INFO_SCALE || chan->type != IIO_INTENSITY)
		return -EINVAL;

	mutex_lock(&dev->lock);
	for (i = 0; i < ARRAY_SIZE(als_scale_tbl); i++) {
		if (als_scale_tbl[i] == val2) {
			ret = regmap_write(dev->regmap, AP3216C_ALS_CONF,
					   (u8)(i << 4));
			break;
		}
	}
	mutex_unlock(&dev->lock);
	return ret;
}

/*
 * 功能：告诉 IIO 核心本驱动期望用户以何种数值格式写入
 * 返回：IIO_VAL_INT_PLUS_MICRO 表示期望 "整数+微数" 格式（如 0.007880）
 * 调用方：IIO 核心在解析用户写入字符串前调用，用于决定怎么把字符串拆成 val/val2
 */
static int ap3216c_write_raw_get_fmt(struct iio_dev *indio_dev,
				     struct iio_chan_spec const *chan,
				     long mask)
{
	return IIO_VAL_INT_PLUS_MICRO;
}

/* IIO 回调集：把上面实现的读/写函数挂给 IIO 框架 */
static const struct iio_info ap3216c_info = {
	.read_raw		= ap3216c_read_raw,
	.write_raw		= ap3216c_write_raw,
	.write_raw_get_fmt	= ap3216c_write_raw_get_fmt,
};

/*
 * 功能：I2C probe——当设备树中 compatible 匹配到本驱动时，内核调用此函数完成设备初始化
 * 参数：client - I2C 客户端实例（内核已根据设备树创建）；id - 传统 I2C ID 表项（常用设备树故忽略）
 * 返回：0 成功，非 0 失败（失败则设备不创建）
 * 调用方：I2C 总线核心（设备树解析/模块加载时）
 */
static int ap3216c_probe(struct i2c_client *client,
			 const struct i2c_device_id *id)
{
	int ret;
	struct ap3216c_dev *dev;
	struct iio_dev *indio_dev;
	struct regmap_config regmap_config = { 0 };	/* regmap 配置：8bit 寄存器地址/8bit 值 */

	/* 1、申请 iio_dev 内存（devm_ 前缀表示设备卸载时自动释放，无需手动 free） */
	indio_dev = devm_iio_device_alloc(&client->dev, sizeof(*dev));
	if (!indio_dev)
		return -ENOMEM;

	dev = iio_priv(indio_dev);	/* 取得跟在 iio_dev 后的私有区首地址 */
	dev->client = client;
	i2c_set_clientdata(client, indio_dev);	/* 关联 client ↔ iio_dev，供 remove 时找回 */

	/* 2、初始化 regmap：用 8bit 寄存器地址/8bit 值的 I2C 模式 */
	regmap_config.reg_bits = 8;
	regmap_config.val_bits = 8;
	dev->regmap = regmap_init_i2c(client, &regmap_config);
	if (IS_ERR(dev->regmap))
		return PTR_ERR(dev->regmap);

	mutex_init(&dev->lock);	/* 初始化互斥锁，保护后续并发的寄存器访问 */

	/* 3、填充 iio_dev 成员：告诉 IIO 框架本设备的能力与回调 */
	indio_dev->dev.parent = &client->dev;
	indio_dev->info = &ap3216c_info;
	indio_dev->name = AP3216C_NAME;
	indio_dev->modes = INDIO_DIRECT_MODE;	/* 直接模式：用户每次 cat 都实时读硬件，而非缓冲 */
	indio_dev->channels = ap3216c_channels;
	indio_dev->num_channels = ARRAY_SIZE(ap3216c_channels);

	/* 4、初始化芯片（先于注册，避免 sysfs 暴露时芯片尚未就绪导致首次读失败） */
	ap3216c_init_chip(dev);

	/* 5、向 IIO 子系统注册设备：成功后 /sys/bus/iio/devices/ 下出现新节点 */
	ret = iio_device_register(indio_dev);
	if (ret) {
		dev_err(&client->dev, "iio_device_register failed\n");
		regmap_exit(dev->regmap);
		return ret;
	}

	dev_info(&client->dev, "AP3216C IIO ready\n");
	return 0;
}

/*
 * 功能：I2C remove——设备拔除或驱动卸载时清理资源
 * 调用方：I2C 总线核心
 */
static int ap3216c_remove(struct i2c_client *client)
{
	struct iio_dev *indio_dev = i2c_get_clientdata(client);
	struct ap3216c_dev *dev = iio_priv(indio_dev);

	iio_device_unregister(indio_dev);	/* 注销 IIO 设备，sysfs 节点消失 */
	regmap_exit(dev->regmap);		/* 释放 regmap 占用的资源 */
	return 0;
}

/* 传统 I2C ID 表：兼容非设备树匹配（保留作备用，实际走 of_match） */
static const struct i2c_device_id ap3216c_id[] = {
	{ "alientek,ap3216c", 0 },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(i2c, ap3216c_id);

/* 设备树匹配表：compatible="alientek,ap3216c" 的节点会绑定本驱动 */
static const struct of_device_id ap3216c_of_match[] = {
	{ .compatible = "alientek,ap3216c" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, ap3216c_of_match);

/* I2C 驱动主体：把 probe/remove 与匹配表挂给 I2C 框架 */
static struct i2c_driver ap3216c_driver = {
	.probe  = ap3216c_probe,
	.remove = ap3216c_remove,
	.driver = {
		.name = AP3216C_NAME,
		.of_match_table = ap3216c_of_match,
	},
	.id_table = ap3216c_id,
};
module_i2c_driver(ap3216c_driver);	/* 宏展开：注册 i2c_driver，并提供 init/exit */

MODULE_LICENSE("GPL");
MODULE_AUTHOR("ALIENTEK / SouthBay");
MODULE_DESCRIPTION("AP3216C ALS/PS/IR IIO driver");
