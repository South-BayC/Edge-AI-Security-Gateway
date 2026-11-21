// SPDX-License-Identifier: GPL-2.0
/*
 * 文件：icm20608.c
 * 所属模块：core/icm20608 — P7-4 ICM20608 六轴 IMU（陀螺仪+加速度计+温度）驱动
 * 作用：把六轴惯性传感器的数据通过 IIO sysfs 暴露给用户态，用于姿态检测、震动监测等
 * 核心功能：
 *   - SPI 读写芯片寄存器，支持陀螺仪(±2000dps)、加速度计(±16G)、温度三类数据
 *   - 温度换算：℃ = raw/326.8 + 25，通过 IIO SCALE/OFFSET 机制让用户 cat 即可得物理值
 *   - WHO_AM_I 校验芯片型号(0xAF=G / 0xAE=D)，量程与滤波在 probe 时一次性配置
 *   - 大端 16bit 数据用 regmap_bulk_read 读取并转主机序
 * 对外接口（用户态怎么用）：
 *   - 无 /dev 节点，走 IIO sysfs：
 *     /sys/bus/iio/devices/iio:deviceX/in_accel_{x,y,z}_raw      # 加速度原始值
 *     /sys/bus/iio/devices/iio:deviceX/in_anglvel_{x,y,z}_raw     # 角速度原始值
 *     /sys/bus/iio/devices/iio:deviceX/in_temp_raw               # 温度原始值
 *     另有 _scale / _offset 节点用于换算物理单位（见 SCALE 表）
 *   - 例：cat in_accel_x_raw；cat in_temp_scale
 * 依赖内核子系统：
 *   - spi_driver（SPI 总线框架，MODE0，匹配 compatible）
 *   - regmap SPI（read_flag_mask=0x80 自动置位读标志，8bit 寄存器/8bit 值）
 *   - iio_dev + IIO sysfs（通道声明与 scale/offset 机制）
 *   - mutex（并发保护）
 *
 * 相对正点原子例程的修正（板级验证）:
 *   - probe 顺序：regmap 初始化先于 iio_device_register（例程相反，存 sysfs 早于 regmap 就绪隐患）
 *   - accel 通道 scan_index 顺序修正为 X/Y/Z
 *   - 温度 OFFSET 修正：raw=0 对应 25℃，offset≈8170（例程 offset=0 导致少 25℃）
 */
#include <linux/module.h>
#include <linux/spi/spi.h>
#include <linux/delay.h>
#include <linux/mutex.h>
#include <linux/regmap.h>
#include <linux/iio/iio.h>
#include <linux/iio/sysfs.h>
#include <linux/iio/buffer.h>
#include <linux/iio/trigger.h>
#include <linux/iio/trigger_consumer.h>
#include <linux/iio/triggered_buffer.h>
#include <linux/hrtimer.h>
#include <linux/ktime.h>

#define ICM20608_NAME "icm20608"	/* 设备名，用于 spi_driver.name 与 iio_dev.name */

/* 寄存器地址表（参考正点原子 icm20608reg.h） */
#define ICM20_SMPLRT_DIV	0x19	/* 采样率分频寄存器 */
#define ICM20_CONFIG		0x1A	/* 陀螺仪低通滤波配置 */
#define ICM20_GYRO_CONFIG	0x1B	/* 陀螺仪量程配置 [4:3] */
#define ICM20_ACCEL_CONFIG	0x1C	/* 加速度计量程配置 [4:3] */
#define ICM20_ACCEL_CONFIG2	0x1D	/* 加速度计低通滤波配置 */
#define ICM20_LP_MODE_CFG	0x1E	/* 低功耗模式配置 */
#define ICM20_PWR_MGMT_2	0x6C	/* 电源管理2：控制各轴使能/休眠 */
#define ICM20_ACCEL_XOUT_H	0x3B	/* 加速度 X 高字节起始（0x3B~0x40 连续 6 字节 XYZ） */
#define ICM20_TEMP_OUT_H	0x41	/* 温度高字节（0x41~0x42） */
#define ICM20_GYRO_XOUT_H	0x43	/* 陀螺仪 X 高字节起始（0x43~0x48 连续 6 字节 XYZ） */
#define ICM20_PWR_MGMT_1	0x6B	/* 电源管理1：复位与时钟源选择 */
#define ICM20_WHO_AM_I		0x75	/* 芯片 ID 寄存器，只读，用于校验型号 */

/* 芯片 ID：区分 ICM20608G(0xAF) 与 D(0xAE)，探针阶段校验 */
#define ICM20608_ID_G		0xAF
#define ICM20608_ID_D		0xAE

/* 关键配置值 */
#define ICM20_PWR_RESET		0x80	/* 软复位：PWR_MGMT_1 bit7=1 */
#define ICM20_PWR_CLK_PLL	0x01	/* 时钟源选 PLL，保证采样稳定 */
#define ICM20_GYRO_2000DPS	0x18	/* 量程 ±2000dps，对应 GYRO_CONFIG[4:3]=11 */
#define ICM20_ACCEL_16G		0x18	/* 量程 ±16G，对应 ACCEL_CONFIG[4:3]=11 */

/* 温度换算推导（小白必看）：
 * 芯片手册公式：温度℃ = raw / 326.8 + 25
 * IIO 框架公式：temp_milli = (raw + OFFSET) * SCALE，单位毫摄氏度
 *   推得 SCALE  = 1000/326.8 ≈ 3.06 milli℃/raw
 *        OFFSET = 25000/3.06 ≈ 8170 raw（保证 raw=0 时 25000m℃=25℃） */
#define ICM20608_TEMP_SCALE_NUM		3	/* SCALE 整数部分 */
#define ICM20608_TEMP_SCALE_DEN		60000	/* SCALE 小数部分：合起来 3.060000，即 3.06（INT_PLUS_MICRO） */
#define ICM20608_TEMP_OFFSET		8170	/* 温度偏移量，修正 25℃ 基准 */

/* 扫描元素索引：与 iio_chan_spec.scan_index 一一对应，预留 buffered 模式使用 */
enum icm20608_scan {
	ICM_SCAN_ACCL_X,	/* 加速度 X */
	ICM_SCAN_ACCL_Y,	/* 加速度 Y */
	ICM_SCAN_ACCL_Z,	/* 加速度 Z */
	ICM_SCAN_TEMP,		/* 温度 */
	ICM_SCAN_GYRO_X,	/* 陀螺仪 X */
	ICM_SCAN_GYRO_Y,	/* 陀螺仪 Y */
	ICM_SCAN_GYRO_Z,	/* 陀螺仪 Z */
};

/* 设备私有数据：每个 SPI 从机一个实例 */
struct icm20608_dev {
	struct spi_device *spi;		/* SPI 客户端句柄 */
	struct regmap *regmap;		/* 寄存器映射，封装 SPI 读写 */
	struct mutex lock;		/* 互斥锁，保护并发的寄存器读 */
	struct iio_trigger *trig;	/* 本设备专属 trigger（buffer 采样节拍） */
	struct hrtimer timer;		/* 周期触发定时器（默认 100Hz） */
	int rate_hz;			/* 采样率 Hz */
};

/* 陀螺仪分辨率表：不同量程下每 LSB 代表多少 dps×1e6，对应 GYRO_CONFIG[4:3]=0..3
 * 例：±250dps 档每 LSB≈0.007629 dps，用户用 raw*scale 即得物理角速度 */
static const int gyro_scale_tbl[] = { 7629, 15258, 30517, 61035 };
/* 加速度计分辨率表：不同量程下每 LSB 代表多少 g×1e9，对应 ACCEL_CONFIG[4:3]=0..3
 * 例：±2g 档每 LSB≈0.000061g */
static const int accel_scale_tbl[] = { 61035, 122070, 244140, 488281 };

/* 三轴通道宏：快速定义陀螺仪/加速度计的单轴通道
 * 参数：_type - IIO 类型；_channel2 - 轴向(X/Y/Z)；_index - 扫描索引
 * 生成的通道含 RAW（每轴独立）与 SCALE（同类型共享，如 3 个加速度轴共用一个量程） */
/* buffered 模式扫描类型：16bit 有符号、主机序（驱动在 push 前已转主机序） */
#define ICM20608_SCAN_TYPE {			\
	.sign = 's',				\
	.realbits = 16,				\
	.storagebits = 16,			\
	.shift = 0,				\
}

#define ICM20608_CHAN(_type, _channel2, _index)			\
	{							\
		.type = _type,					\
		.modified = 1,					\
		.channel2 = _channel2,				\
		.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE),	\
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),	\
		.scan_index = _index,				\
		.scan_type = ICM20608_SCAN_TYPE,		\
	}

static const struct iio_chan_spec icm20608_channels[] = {
	{	/* 温度 */
		.type = IIO_TEMP,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |
				      BIT(IIO_CHAN_INFO_OFFSET) |
				      BIT(IIO_CHAN_INFO_SCALE),
		.scan_index = ICM_SCAN_TEMP,
		.scan_type = ICM20608_SCAN_TYPE,
	},
	ICM20608_CHAN(IIO_ANGL_VEL, IIO_MOD_X, ICM_SCAN_GYRO_X),	/* 陀螺仪 X */
	ICM20608_CHAN(IIO_ANGL_VEL, IIO_MOD_Y, ICM_SCAN_GYRO_Y),	/* 陀螺仪 Y */
	ICM20608_CHAN(IIO_ANGL_VEL, IIO_MOD_Z, ICM_SCAN_GYRO_Z),	/* 陀螺仪 Z */
	ICM20608_CHAN(IIO_ACCEL, IIO_MOD_X, ICM_SCAN_ACCL_X),		/* 加速度 X */
	ICM20608_CHAN(IIO_ACCEL, IIO_MOD_Y, ICM_SCAN_ACCL_Y),		/* 加速度 Y */
	ICM20608_CHAN(IIO_ACCEL, IIO_MOD_Z, ICM_SCAN_ACCL_Z),		/* 加速度 Z */
};

/*
 * 功能：读单个 16bit 有符号通道数据（通用 helper）
 * 参数：dev - 设备私有结构；reg - 该类型 X 轴高字节基地址；axis - IIO_MOD_X/Y/Z；val - 输出有符号原始值
 * 返回：IIO_VAL_INT 表示 *val 有效，负值表错
 * 调用方：icm20608_read_channel_data()
 * 原理：reg + (axis-X)*2 定位到对应轴的高字节；用 regmap_bulk_read 一次读 2 字节（大端），转主机序后强转为 short 保留符号
 */
static int icm20608_sensor_show(struct icm20608_dev *dev, u8 reg, int axis,
				int *val)
{
	__be16 d;
	int ret;

	ret = regmap_bulk_read(dev->regmap, reg + (axis - IIO_MOD_X) * 2,
			       (u8 *)&d, 2);
	if (ret)
		return -EINVAL;
	*val = (short)be16_to_cpup(&d);
	return IIO_VAL_INT;
}

/*
 * 功能：根据通道类型分发到具体寄存器基地址
 * 参数：indio_dev - IIO 设备；chan - 通道描述；val - 输出原始值
 * 返回：IIO_VAL_INT 成功，-EINVAL 类型不支持
 * 调用方：icm20608_read_raw() 的 RAW 分支
 */
static int icm20608_read_channel_data(struct iio_dev *indio_dev,
				      struct iio_chan_spec const *chan,
				      int *val)
{
	struct icm20608_dev *dev = iio_priv(indio_dev);

	switch (chan->type) {
	case IIO_ANGL_VEL:
		return icm20608_sensor_show(dev, ICM20_GYRO_XOUT_H,
					    chan->channel2, val);
	case IIO_ACCEL:
		return icm20608_sensor_show(dev, ICM20_ACCEL_XOUT_H,
					    chan->channel2, val);
	case IIO_TEMP:
		return icm20608_sensor_show(dev, ICM20_TEMP_OUT_H,
					    IIO_MOD_X, val);
	default:
		return -EINVAL;
	}
}

/*
 * 功能：IIO 框架回调——用户读 sysfs 节点时被调用（核心分发函数）
 * 参数：indio_dev - IIO 设备；chan - 哪个通道；val/val2 - 输出；mask - 要读 RAW 还是 SCALE/OFFSET
 * 返回：IIO_VAL_* 表示 val/val2 的解读方式，负值表错
 * 调用方：IIO 核心（用户 cat 各种 _raw/_scale/_offset 时）
 * 关键逻辑：RAW 需加锁后读寄存器；SCALE 根据量程寄存器查表；温度 OFFSET 直接返回常量
 */
static int icm20608_read_raw(struct iio_dev *indio_dev,
			     struct iio_chan_spec const *chan,
			     int *val, int *val2, long mask)
{
	struct icm20608_dev *dev = iio_priv(indio_dev);
	unsigned int regdata;
	int ret = 0;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:	/* 读原始值：加锁后走通道分发 */
		mutex_lock(&dev->lock);
		ret = icm20608_read_channel_data(indio_dev, chan, val);
		mutex_unlock(&dev->lock);
		return ret;

	case IIO_CHAN_INFO_SCALE:	/* 读量程分辨率：用户用 raw*scale 得物理值 */
		switch (chan->type) {
		case IIO_ANGL_VEL:	/* 陀螺仪：读 GYRO_CONFIG[4:3] 查 gyro_scale_tbl，单位 dps×1e6 */
			mutex_lock(&dev->lock);
			ret = regmap_read(dev->regmap, ICM20_GYRO_CONFIG, &regdata);
			mutex_unlock(&dev->lock);
			if (ret)
				return ret;
			*val = 0;
			*val2 = gyro_scale_tbl[(regdata & 0x18) >> 3];
			return IIO_VAL_INT_PLUS_MICRO;
		case IIO_ACCEL:		/* 加速度：读 ACCEL_CONFIG[4:3] 查 accel_scale_tbl，单位 g×1e9 */
			mutex_lock(&dev->lock);
			ret = regmap_read(dev->regmap, ICM20_ACCEL_CONFIG, &regdata);
			mutex_unlock(&dev->lock);
			if (ret)
				return ret;
			*val = 0;
			*val2 = accel_scale_tbl[(regdata & 0x18) >> 3];
			return IIO_VAL_INT_PLUS_NANO;
		case IIO_TEMP:		/* 温度：固定 3.06 milli℃/raw，无需查表 */
			*val = ICM20608_TEMP_SCALE_NUM;
			*val2 = ICM20608_TEMP_SCALE_DEN;
			return IIO_VAL_INT_PLUS_MICRO;
		default:
			return -EINVAL;
		}

	case IIO_CHAN_INFO_OFFSET:	/* 温度偏移：仅温度通道有，raw=0 对应 25℃ */
		if (chan->type != IIO_TEMP)
			return -EINVAL;
		*val = ICM20608_TEMP_OFFSET;
		return IIO_VAL_INT;

	default:
		return -EINVAL;
	}
}

/* IIO 回调集：仅需 read_raw，本驱动量程固定不可写 */
static const struct iio_info icm20608_info = {
	.read_raw		= icm20608_read_raw,
};

/* ================================================================
 * Buffered 模式（T3-B）：hrtimer 周期触发 + IIO triggered buffer
 *
 * 数据布局按 scan_index：
 *   data[0..2] = ACCEL X/Y/Z
 *   data[3]    = TEMP
 *   data[4..6] = GYRO X/Y/Z
 *   timestamp  = 8 字节对齐
 * 寄存器 0x3B 起连续 14 字节的顺序正好与 scan_index 一致。
 * ================================================================ */
#define ICM20608_DEFAULT_RATE_HZ 100

struct icm20608_buffer {
	s16 data[8];		/* 7 通道 + padding；scan_bytes=14，时间戳对齐到 16 */
	s64 timestamp __aligned(8);
};

static irqreturn_t icm20608_trigger_handler(int irq, void *p)
{
	struct iio_poll_func *pf = p;
	struct iio_dev *indio_dev = pf->indio_dev;
	struct icm20608_dev *dev = iio_priv(indio_dev);
	__be16 raw[7];
	struct icm20608_buffer buf;
	int ret, i;

	memset(&buf, 0, sizeof(buf));

	mutex_lock(&dev->lock);
	ret = regmap_bulk_read(dev->regmap, ICM20_ACCEL_XOUT_H,
			       raw, sizeof(raw));
	mutex_unlock(&dev->lock);

	if (ret == 0) {
		for (i = 0; i < 7; i++)
			buf.data[i] = (s16)be16_to_cpup(&raw[i]);
		iio_push_to_buffers_with_timestamp(indio_dev, &buf,
						   iio_get_time_ns());
	}

	iio_trigger_notify_done(indio_dev->trig);
	return IRQ_HANDLED;
}

static enum hrtimer_restart icm20608_timer_handler(struct hrtimer *timer)
{
	struct icm20608_dev *dev = container_of(timer, struct icm20608_dev,
						timer);

	iio_trigger_poll(dev->trig);
	hrtimer_forward_now(timer, ms_to_ktime(1000 / dev->rate_hz));
	return HRTIMER_RESTART;
}

static int icm20608_buffer_postenable(struct iio_dev *indio_dev)
{
	struct icm20608_dev *dev = iio_priv(indio_dev);
	int ret;

	/* 先让 IIO core 把 pollfunc attach 到 trigger，再启动采样节拍 */
	ret = iio_triggered_buffer_postenable(indio_dev);
	if (ret)
		return ret;

	hrtimer_start(&dev->timer, ms_to_ktime(1000 / dev->rate_hz),
		      HRTIMER_MODE_REL);
	return 0;
}

static int icm20608_buffer_predisable(struct iio_dev *indio_dev)
{
	struct icm20608_dev *dev = iio_priv(indio_dev);

	hrtimer_cancel(&dev->timer);
	return iio_triggered_buffer_predisable(indio_dev);
}

static const struct iio_buffer_setup_ops icm20608_buffer_ops = {
	.postenable	= icm20608_buffer_postenable,
	.predisable	= icm20608_buffer_predisable,
};

/* trigger 操作集：iio_trigger_get()/put() 会访问 ops->owner，必须设置；
 * 采样节拍由驱动内 hrtimer 控制，不需要 set_trigger_state。 */
static const struct iio_trigger_ops icm20608_trigger_ops = {
	.owner = THIS_MODULE,
};

/*
 * 功能：SPI probe——设备树匹配到本驱动时初始化芯片与 IIO 设备
 * 参数：spi - SPI 客户端实例
 * 返回：0 成功，非 0 失败
 * 调用方：SPI 总线核心
 */
static int icm20608_probe(struct spi_device *spi)
{
	int ret;
	unsigned int who;	/* 芯片 ID 读取值 */
	struct icm20608_dev *dev;
	struct iio_dev *indio_dev;
	struct regmap_config regmap_config = { 0 };

	/* 1、申请 iio_dev 内存（devm_ 自动释放） */
	indio_dev = devm_iio_device_alloc(&spi->dev, sizeof(*dev));
	if (!indio_dev)
		return -ENOMEM;

	dev = iio_priv(indio_dev);
	dev->spi = spi;
	spi_set_drvdata(spi, indio_dev);	/* 关联 spi ↔ iio_dev，供 remove 找回 */
	mutex_init(&dev->lock);

	/* 2、初始化 regmap（SPI 模式）：ICM20608 读时需把地址 bit7 置 1，由 read_flag_mask 自动处理 */
	regmap_config.reg_bits = 8;
	regmap_config.val_bits = 8;
	regmap_config.read_flag_mask = 0x80;
	dev->regmap = regmap_init_spi(spi, &regmap_config);
	if (IS_ERR(dev->regmap))
		return PTR_ERR(dev->regmap);

	/* 3、配置 SPI 模式为 MODE0（CPOL=0,CPHA=0），并应用 */
	spi->mode = SPI_MODE_0;
	spi_setup(spi);

	/* 4、芯片上电初始化：软复位→切 PLL 时钟→校验 ID→配置量程与滤波 */
	regmap_write(dev->regmap, ICM20_PWR_MGMT_1, ICM20_PWR_RESET);	/* 软复位 */
	msleep(50);	/* 复位后需等待芯片就绪 */
	regmap_write(dev->regmap, ICM20_PWR_MGMT_1, ICM20_PWR_CLK_PLL);	/* 切 PLL 时钟源更稳定 */
	msleep(50);

	/* 校验 WHO_AM_I，防止贴错芯片或 SPI 线序错导致后续配置无效 */
	ret = regmap_read(dev->regmap, ICM20_WHO_AM_I, &who);
	if (ret || (who != ICM20608_ID_G && who != ICM20608_ID_D)) {
		dev_err(&spi->dev, "WHO_AM_I mismatch: 0x%02x\n", who);
		regmap_exit(dev->regmap);
		return ret ? ret : -ENODEV;
	}
	dev_info(&spi->dev, "ICM20608 WHO_AM_I = 0x%02x\n", who);

	regmap_write(dev->regmap, ICM20_SMPLRT_DIV, 0x00);	/* 采样分频 0，即最高采样率 */
	regmap_write(dev->regmap, ICM20_GYRO_CONFIG, ICM20_GYRO_2000DPS);	/* 陀螺仪 ±2000dps */
	regmap_write(dev->regmap, ICM20_ACCEL_CONFIG, ICM20_ACCEL_16G);		/* 加速度 ±16G */
	regmap_write(dev->regmap, ICM20_CONFIG, 0x04);		/* 陀螺低通滤波 20Hz，滤高频抖动 */
	regmap_write(dev->regmap, ICM20_ACCEL_CONFIG2, 0x04);	/* 加速度低通 21.2Hz */
	regmap_write(dev->regmap, ICM20_PWR_MGMT_2, 0x00);	/* 使能全部 6 轴（不休眠） */
	regmap_write(dev->regmap, ICM20_LP_MODE_CFG, 0x00);	/* 关闭低功耗模式，保证连续采样 */

	/* 5、填充 iio_dev 成员（regmap 就绪后再注册，避免 sysfs 早于硬件就绪被用户读到） */
	indio_dev->dev.parent = &spi->dev;
	indio_dev->info = &icm20608_info;
	indio_dev->name = ICM20608_NAME;
	indio_dev->modes = INDIO_DIRECT_MODE | INDIO_BUFFER_TRIGGERED;
	indio_dev->channels = icm20608_channels;
	indio_dev->num_channels = ARRAY_SIZE(icm20608_channels);

	/* 6、buffered 模式：hrtimer 周期触发 + IIO triggered buffer */
	dev->rate_hz = ICM20608_DEFAULT_RATE_HZ;
	hrtimer_init(&dev->timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	dev->timer.function = icm20608_timer_handler;

	dev->trig = iio_trigger_alloc("icm20608-dev%d", spi->master->bus_num);
	if (!dev->trig) {
		dev_err(&spi->dev, "iio_trigger_alloc failed\n");
		regmap_exit(dev->regmap);
		return -ENOMEM;
	}
	dev->trig->dev.parent = &spi->dev;
	dev->trig->ops = &icm20608_trigger_ops;

	ret = iio_trigger_register(dev->trig);
	if (ret) {
		dev_err(&spi->dev, "iio_trigger_register failed: %d\n", ret);
		iio_trigger_free(dev->trig);
		dev->trig = NULL;
		regmap_exit(dev->regmap);
		return ret;
	}

	ret = iio_triggered_buffer_setup(indio_dev, NULL,
					 icm20608_trigger_handler,
					 &icm20608_buffer_ops);
	if (ret) {
		dev_err(&spi->dev, "iio_triggered_buffer_setup failed: %d\n", ret);
		iio_trigger_unregister(dev->trig);
		iio_trigger_free(dev->trig);
		dev->trig = NULL;
		regmap_exit(dev->regmap);
		return ret;
	}

	/* 4.1.15 标准绑定：设置设备 trigger 并增加引用。
	 * pollfunc 不在 probe 里 attach，而是在 buffer enable 时由
	 * iio_triggered_buffer_postenable() 自动 attach。 */
	indio_dev->trig = iio_trigger_get(dev->trig);

	/* 7、向 IIO 子系统注册，成功后 sysfs/buffer 节点出现 */
	ret = iio_device_register(indio_dev);
	if (ret) {
		dev_err(&spi->dev, "iio_device_register failed\n");
		iio_triggered_buffer_cleanup(indio_dev);
		iio_trigger_unregister(dev->trig);
		iio_trigger_free(dev->trig);
		dev->trig = NULL;
		regmap_exit(dev->regmap);
		return ret;
	}

	dev_info(&spi->dev, "ICM20608 IIO ready (buffered %dHz)\n",
		 dev->rate_hz);
	return 0;
}

/*
 * 功能：SPI remove——卸载时注销 IIO 设备并释放 regmap
 * 调用方：SPI 总线核心
 */
static int icm20608_remove(struct spi_device *spi)
{
	struct iio_dev *indio_dev = spi_get_drvdata(spi);
	struct icm20608_dev *dev = iio_priv(indio_dev);

	iio_device_unregister(indio_dev);	/* 摘 sysfs/buffer 节点 */
	iio_triggered_buffer_cleanup(indio_dev);
	if (dev->trig) {
		iio_trigger_unregister(dev->trig);
		iio_trigger_free(dev->trig);
		dev->trig = NULL;
	}
	hrtimer_cancel(&dev->timer);
	regmap_exit(dev->regmap);		/* 释放 regmap */
	return 0;
}

/* 传统 SPI ID 表（备用，实际走设备树） */
static const struct spi_device_id icm20608_id[] = {
	{ "alientek,icm20608", 0 },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(spi, icm20608_id);

/* 设备树匹配表：compatible="alientek,icm20608" 的节点绑定本驱动 */
static const struct of_device_id icm20608_of_match[] = {
	{ .compatible = "alientek,icm20608" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, icm20608_of_match);

/* SPI 驱动主体 */
static struct spi_driver icm20608_driver = {
	.probe  = icm20608_probe,
	.remove = icm20608_remove,
	.driver = {
		.name = ICM20608_NAME,
		.of_match_table = icm20608_of_match,
	},
	.id_table = icm20608_id,
};
module_spi_driver(icm20608_driver);	/* 注册 spi_driver */

MODULE_LICENSE("GPL");
MODULE_AUTHOR("ALIENTEK / SouthBay");
MODULE_DESCRIPTION("ICM20608 gyro/accel/temp IIO driver");
