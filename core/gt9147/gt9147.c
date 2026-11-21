/* ------------------------------------------------------------------
 * 文件: gt9147/gt9147.c
 * 模块: GT9147 电容触摸屏驱动（P7-1，中断模式，1024x600，含配置自愈）
 * 作用: 驱动 Goodix GT9147/GT911 触摸芯片，把 I2C 触摸数据转为标准
 *       Linux input 事件，供 LVGL/Qt/evdev 直接消费。
 * 核心功能:
 *   - GPIO 复位时序 + I2C 寄存器读写（双消息读、单消息写）
 *   - 固件配置下载自愈：芯片空白(全零)时下载官方验证配置表(184字节+
 *     校验和+fresh)，重读后获得正确 max_x/max_y/中断极性
 *   - 中断模式报点：threaded IRQ → 读 GSTID → 读坐标 → input_mt_slot 上报
 *   - 中断风暴/读取失败自愈：连续失败计次达阈值则软复位芯片
 *   - input 子系统多点触摸上报（ABS_MT + slot 协议）
 * 对外接口（用户态怎么用）:
 *   - 设备节点: /dev/input/eventX（input 子系统自动创建，X 由内核分配）
 *   - 读取: evtest /dev/input/eventX 或 LVGL evdev 驱动直接消费
 *   - 区分设备: /proc/bus/input/devices 找 "gt9147"
 *   - 设备树需配 interrupt-gpios/reset-gpios/compatible="goodix,gt9147"
 * 依赖的内核机制:
 *   - i2c 驱动框架 i2c_driver + i2c_client + i2c_transfer
 *   - gpio 消费者接口 + devm_gpio_request_one / gpio_set_value
 *   - 中断子系统 devm_request_threaded_irq + IRQF_ONESHOT
 *   - input 子系统 devm_input_allocate_device / input_mt_init_slots /
 *     input_mt_slot / input_mt_report_slot_state / input_report_abs /
 *     input_mt_report_pointer_emulation / input_sync
 *   - 设备树 of_get_named_gpio / of_irq
 * 硬件要点: GT9147 寄存器地址 16 位，需先写地址再读数据；配置区
 *           GT_9xx_CFGS_REG 起 184 字节本体 + 校验和 + fresh 标志
 * ------------------------------------------------------------------ */
#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/regmap.h>
#include <linux/gpio/consumer.h>
#include <linux/of_irq.h>
#include <linux/interrupt.h>
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/gpio.h>
#include <linux/of_gpio.h>
#include <linux/input/mt.h>
#include <linux/input/touchscreen.h>
#include <linux/i2c.h>
#include <asm/unaligned.h>

/* GT9147 寄存器地址表（16 位地址，通过 I2C 读写） */
#define GT_CTRL_REG 	        0X8040  /* 控制寄存器（写 0x02 软复位）         */
#define GT_MODSW_REG 	        0X804D  /* 模式切换寄存器        */
#define GT_9xx_CFGS_REG 	        0X8047  /* GT9xx 配置起始地址（184 字节本体起点）    */
#define GT_1xx_CFGS_REG 	        0X8050  /* GT1151 配置起始地址    */
#define GT_CHECK_REG 	        0X80FF  /* 校验和寄存器       */
#define GT_PID_REG 		        0X8140  /* 产品 ID 寄存器（4 字节 ASCII + 版本）       */

#define GT_GSTID_REG 	        0X814E  /* 触摸状态寄存器（低 4 位为触摸点数） */
#define GT_TP1_REG 		        0X814F  /* 触摸点1 数据地址（5 字节：id/x/y） */
#define GT_TP2_REG 		        0X8157	/* 触摸点2 数据地址 */
#define GT_TP3_REG 		        0X815F  /* 触摸点3 数据地址 */
#define GT_TP4_REG 		        0X8167  /* 触摸点4 数据地址  */
#define GT_TP5_REG 		        0X816F	/* 触摸点5 数据地址   */
#define MAX_SUPPORT_POINTS      5       /* 硬件最多支持 5 点同时触摸 */

/* ===== SouthBay 板适配：官方验证配置表 =====
 * 来源: 正点原子裸机 bsp_gt9147.c GT9147_CFG_TBL（本屏家族验证配置），
 * X/Y 补丁为 7寸 1024x600（0x00,0x04 / 0x58,0x02）；
 * [6]=0x0d → &0x3=1 → 下降沿中断（与 irq_table 匹配）。
 * 本板芯片出厂配置区实测为全零，probe 检测到空白时下载本表。
 * 配置长度 184 字节本体 + 2 字节（校验和 + fresh），共 186 字节写入 GT_9xx_CFGS_REG */
#define GT9147_CFG_BODY	184	/* 配置本体长度（不含校验和与 fresh 标志） */

static const u8 gt9147_cfg_tbl[GT9147_CFG_BODY] = {
	0x41, 0x00, 0x04, 0x58, 0x02, 0x05, 0x0d, 0x00, 0x01, 0x08,
	0x28, 0x05, 0x50, 0x32, 0x03, 0x05, 0x00, 0x00, 0xff, 0xff,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x89, 0x28, 0x0a,
	0x17, 0x15, 0x31, 0x0d, 0x00, 0x00, 0x02, 0x9b, 0x03, 0x25,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x32, 0x00, 0x00,
	0x00, 0x0f, 0x94, 0x94, 0xc5, 0x02, 0x07, 0x00, 0x00, 0x04,
	0x8d, 0x13, 0x00, 0x5c, 0x1e, 0x00, 0x3c, 0x30, 0x00, 0x29,
	0x4c, 0x00, 0x1e, 0x78, 0x00, 0x1e, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x08, 0x0a, 0x0c, 0x0e, 0x10, 0x12, 0x14, 0x16,
	0x18, 0x1a, 0x00, 0x00, 0x00, 0x00, 0x1f, 0xff, 0xff, 0xff,
	0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
	0xff, 0xff, 0x00, 0x02, 0x04, 0x05, 0x06, 0x08, 0x0a, 0x0c,
	0x0e, 0x1d, 0x1e, 0x1f, 0x20, 0x22, 0x24, 0x28, 0x29, 0xff,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff,
	0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
	0xff, 0xff, 0xff, 0xff,
};

/* 驱动私有数据（全局单例，I2C 触摸单实例）
 * - irq_pin/reset_pin: 设备树解析的 GPIO 号（interrupt-gpios/reset-gpios）
 * - irqnum: 中断号（client->irq）；irqtype: 触发类型（存 data[6]&0x3）
 * - max_x/max_y: 屏幕分辨率（从固件配置读出，用于 input 绝对坐标范围）
 * - input: input 子系统设备对象
 * - client: I2C 客户端（与芯片通信的句柄）
 * - irq_fail_cnt: 连续 I2C 读取失败计数（≥5 触发软复位，防风暴）
 * - irq_no_data_cnt: 连续中断无数据计数（≥30 触发软复位，防 INT 异常风暴） */
struct gt9147_dev {
	int irq_pin,reset_pin;					/* 中断和复位 GPIO 号		*/
	int irqnum;								/* 中断号    		*/
	int irqtype;							/* 中断类型（RISING/FALLING 等）         */
	int max_x;								/* 最大横坐标（分辨率宽）   	*/
	int max_y; 								/* 最大纵坐标（分辨率高）		*/
	void *private_data;						/* 私有数据（预留） 		*/
	struct input_dev *input;				/* input 设备结构体 		*/
	struct i2c_client *client;				/* I2C 客户端句柄 		*/
	int irq_fail_cnt;						/* 中断读取失败连续计数（防风暴） */
	int irq_no_data_cnt;					/* 中断无数据连续计数（防风暴） */
};
/* 全局设备对象：单芯片场景简化为全局变量，多实例需改为动态分配 */
struct gt9147_dev gt9147;

/* 中断触发方式表：索引对应固件配置字节 data[6]&0x3，用于 request_irq 的 flags */
const u8 irq_table[] = {IRQ_TYPE_EDGE_RISING, IRQ_TYPE_EDGE_FALLING, IRQ_TYPE_LEVEL_LOW, IRQ_TYPE_LEVEL_HIGH};

/*
 * 复位 GT9147（硬件复位时序）
 * 功能: 申请复位/中断 GPIO 并按芯片手册时序拉低复位、拉低 INT、再切输入
 * 参数: client - I2C 客户端；dev - 驱动私有数据（含 irq_pin/reset_pin）
 * 返回: 0 成功，负值失败
 * 被谁调用: probe 阶段初始化
 * 时序要点: 复位低 10ms → 高 10ms → INT 低 50ms → 切输入，严格按手册否则不响应
 */
static int gt9147_ts_reset(struct i2c_client *client, struct gt9147_dev *dev)
{
	int ret = 0;

    /* 申请复位IO*/
	if (gpio_is_valid(dev->reset_pin)) {  		
		/* 申请复位IO，并且默认输出高电平 */
		ret = devm_gpio_request_one(&client->dev,	
					dev->reset_pin, GPIOF_OUT_INIT_HIGH,
					"gt9147 reset");
		if (ret) {
			return ret;
		}
	}

    /* 申请中断IO*/
	if (gpio_is_valid(dev->irq_pin)) {  		
		/* 申请复位IO，并且默认输出高电平 */
		ret = devm_gpio_request_one(&client->dev,	
					dev->irq_pin, GPIOF_OUT_INIT_HIGH,
					"gt9147 int");
		if (ret) {
			return ret;
		}
	}

    /* 4、初始化GT9147，要严格按照GT9147时序要求 */
    gpio_set_value(dev->reset_pin, 0); /* 复位GT9147 */
    msleep(10);
    gpio_set_value(dev->reset_pin, 1); /* 停止复位GT9147 */
    msleep(10);
    gpio_set_value(dev->irq_pin, 0);    /* 拉低INT引脚 */
    msleep(50);
    gpio_direction_input(dev->irq_pin); /* INT引脚设置为输入 */

	return 0;
}

/* 软复位 GT9147（中断异常/风暴时自愈调用）
 * 功能: 通过控制寄存器 0x8040 写 0x02 触发软复位，无需 GPIO 硬复位，避免重配 pinctrl
 * 被谁调用: irq_handler 连续失败/风暴时 */
static s32 gt9147_write_regs(struct gt9147_dev *dev, u16 reg, u8 *buf, u8 len); /* 前向声明：写寄存器 */

static int gt9147_ts_soft_reset(struct gt9147_dev *dev)
{
	u8 data = 0x02;		/* 0x8040 写 0x02 = 软复位命令 */
	gt9147_write_regs(dev, GT_CTRL_REG, &data, 1);
	mdelay(100);
	data = 0x00;		/* 写 0x00 停止软复位，恢复正常 */
	gt9147_write_regs(dev, GT_CTRL_REG, &data, 1);
	mdelay(100);
	return 0;
}

/*
 * 从 GT9147 读取多个寄存器（I2C 双消息事务）
 * 功能: 先写 2 字节寄存器地址，再读 len 字节数据
 * 参数: dev - 设备对象；reg - 起始寄存器地址；buf - 接收缓冲；len - 长度
 * 返回: 0 成功，-EREMOTEIO 失败
 * 被谁调用: irq_handler / read_firmware / probe 等
 * 内核机制: i2c_transfer 双消息（写地址+读数据），GT9147 地址为 16 位大端
 */
static int gt9147_read_regs(struct gt9147_dev *dev, u16 reg, u8 *buf, int len)
{
	int ret;
    u8 regdata[2];
	struct i2c_msg msg[2];
	struct i2c_client *client = (struct i2c_client *)dev->client;
    
    /* GT9147寄存器长度为2个字节 */
    regdata[0] = reg >> 8;
    regdata[1] = reg & 0xFF;

	/* msg[0]为发送要读取的首地址 */
	msg[0].addr = client->addr;			/* ft5x06地址 */
	msg[0].flags = !I2C_M_RD;			/* 标记为发送数据 */
	msg[0].buf = &regdata[0];			/* 读取的首地址 */
	msg[0].len = 2;						/* reg长度*/

	/* msg[1]读取数据 */
	msg[1].addr = client->addr;			/* ft5x06地址 */
	msg[1].flags = I2C_M_RD;			/* 标记为读取数据*/
	msg[1].buf = buf;					/* 读取数据缓冲区 */
	msg[1].len = len;					/* 要读取的数据长度*/

	ret = i2c_transfer(client->adapter, msg, 2);
	if(ret == 2) {
		ret = 0;
	} else {
		ret = -EREMOTEIO;
	}
	return ret;
}

/*
 * 向 GT9147 写入多个寄存器（I2C 单消息事务）
 * 功能: 组包“2 字节地址 + 数据”后一次性写出
 * 参数: dev - 设备对象；reg - 起始地址；buf - 待写数据；len - 长度
 * 返回: i2c_transfer 返回值（1 成功，负值失败）
 * 被谁调用: 软复位、配置下载、清 GSTID 等
 * 细节: 内部用 256 字节栈缓冲拼地址+数据，故 len 不宜过大
 */
static s32 gt9147_write_regs(struct gt9147_dev *dev, u16 reg, u8 *buf, u8 len)
{
	u8 b[256];
	struct i2c_msg msg;
	struct i2c_client *client = (struct i2c_client *)dev->client;
	
	b[0] = reg >> 8;			/* 寄存器首地址低8位 */
    b[1] = reg & 0XFF;			/* 寄存器首地址高8位 */
	memcpy(&b[2],buf,len);		/* 将要写入的数据拷贝到数组b里面 */

	msg.addr = client->addr;	/* gt9147地址 */
	msg.flags = 0;				/* 标记为写数据 */

	msg.buf = b;				/* 要写入的数据缓冲区 */
	msg.len = len + 2;			/* 要写入的数据长度 */

	return i2c_transfer(client->adapter, &msg, 1);
}

/* 中断回调：触摸中断触发时被内核调用（threaded IRQ，可睡眠）
 * 功能: 读触摸状态→解析坐标→通过 input 子系统上报按/释事件
 * 参数: irq - 中断号；dev_id - 指向 gt9147_dev
 * 返回: IRQ_HANDLED
 * 上下文: 内核线程化中断上下文（devm_request_threaded_irq 顶半为 NULL，底半可睡眠，故可用 I2C）
 * 被谁调用: 内核中断线程
 * 关键逻辑:
 *  - GSTID 读失败/无数据连续计数达阈值则软复位自愈，防 I2C 异常与中断风暴
 *  - 单点触摸：touch_num 非 0 读 TP1 坐标并上报 slot 状态 true，0 则上报 false
 *  - 报点结束需写 GSTID 0 清标志，否则芯片持续拉中断 */
static irqreturn_t gt9147_irq_handler(int irq, void *dev_id)
{
    int touch_num = 0;
    int input_x, input_y;
    int id = 0;
    int ret = 0;
    u8 data;
    u8 touch_data[5];
    struct gt9147_dev *dev = dev_id;

    /* 读触摸状态寄存器 GSTID：低 4 位为当前触摸点数，bit7 为 buffer 状态 */
    ret = gt9147_read_regs(dev, GT_GSTID_REG, &data, 1);
    if (ret) {
        /* I2C 读取失败：连续 5 次则软复位芯片，避免中断风暴空转（I2C 掉线时中断仍频繁触发） */
        if (++gt9147.irq_fail_cnt >= 5) {
            gt9147.irq_fail_cnt = 0;
            gt9147_ts_soft_reset(dev);
            dev_err(&dev->client->dev, "irq read fail x5, soft reset\n");
        }
        goto fail;
    }
    gt9147.irq_fail_cnt = 0;

    if (data == 0x00)  {     /* 没有触摸数据，直接返回（可能为误触发） */
        /* 中断风暴保护：连续 30 次“中断但无触摸数据”说明芯片 INT 引脚异常，
         * 主动软复位恢复，避免持续空转（现象为 /proc/interrupts 计数暴涨但无报点） */
        if (++gt9147.irq_no_data_cnt >= 30) {
            gt9147.irq_no_data_cnt = 0;
            gt9147_ts_soft_reset(dev);
            dev_err(&dev->client->dev, "irq storm (no-data x30), soft reset\n");
        }
        goto fail;
    } else {                 /* 有触摸点，解析点数 */
        gt9147.irq_no_data_cnt = 0;
        touch_num = data & 0x0f; /* 低 4 位为点数 */
    }

    /* GT9147 无硬件抬起/按下独立事件，暂用单点逻辑：touch_num>0 视为按下，0 视为释放
     * 多点可扩展为遍历各 TP 寄存器，此处简化保证稳定性 */
    if(touch_num) {         /* 单点触摸按下：读点1 坐标并上报 */
        gt9147_read_regs(dev, GT_TP1_REG, touch_data, 5);
        id = touch_data[0] & 0x0F; /* 低 4 位为触摸 ID */
        if(id == 0) {
            /* 坐标为小端 16 位：低字节 | 高字节<<8 */
            input_x  = touch_data[1] | (touch_data[2] << 8);
            input_y  = touch_data[3] | (touch_data[4] << 8);

            /* input 多点上报：选 slot 0，标记为手指按下，上报 X/Y 绝对坐标
             * slot 协议让上层可区分多指；此处单点固定 id 0 */
            input_mt_slot(dev->input, id);
		    input_mt_report_slot_state(dev->input, MT_TOOL_FINGER, true);
		    input_report_abs(dev->input, ABS_MT_POSITION_X, input_x);
		    input_report_abs(dev->input, ABS_MT_POSITION_Y, input_y);
        }
    } else if(touch_num == 0){                /* 单点触摸释放：标记 slot 0 抬起 */
        input_mt_slot(dev->input, id);
        input_mt_report_slot_state(dev->input, MT_TOOL_FINGER, false);
    }

    /* 兼容旧单点协议：把多点 slot 转为单点 ABS_X/Y + BTN_TOUCH，供不支持 slot 的上层使用 */
	input_mt_report_pointer_emulation(dev->input, true);
    /* 同步：告知 input 子系统本批事件结束，触发向用户态发送完整事件帧 */
    input_sync(dev->input);

    data = 0x00;                /* 向 0x814E 写 0 清 buffer 标志，芯片才会在下次触摸再拉中断 */
    gt9147_write_regs(dev, GT_GSTID_REG, &data, 1);

fail:
	return IRQ_HANDLED;
}


/*
 * GT9147 中断初始化（threaded IRQ）
 * 功能: 申请线程化中断，触发方式由固件配置决定（上升/下降沿等）
 * 参数: client - I2C 客户端；dev - 私有数据（含 irqtype）
 * 返回: 0 成功，负值失败
 * 被谁调用: probe 在 input 注册后调用
 * 细节: 顶半 handler 传 NULL（无需快速处理），底半 gt9147_irq_handler 为可睡眠线程；
 *       IRQF_ONESHOT 保证中断线程执行期间屏蔽同中断，避免重入
 */
static int gt9147_ts_irq(struct i2c_client *client, struct gt9147_dev *dev)
{
	int ret = 0;
	/* 申请线程化中断：client->irq 来自设备树 interrupt 映射；flags 查 irq_table[irqtype] */
	ret = devm_request_threaded_irq(&client->dev, client->irq, NULL,
					gt9147_irq_handler, irq_table[dev->irqtype] | IRQF_ONESHOT,
					client->name, &gt9147);
	if (ret) {
		dev_err(&client->dev, "Unable to request touchscreen IRQ.\n");
		return ret;
	}

	return 0;
}

/*
 * 读取 GT9147 固件信息与配置（分辨率与中断极性）
 * 功能: 读 PID/版本，依芯片型号选配置区，解析出 max_x/max_y/irqtype
 * 参数: client - I2C 客户端；dev - 私有数据（输出 max_x/max_y/irqtype）
 * 返回: 0 成功，负值失败
 * 被谁调用: probe 初始化与配置下载后重读
 * 细节: GT1151/GT5663 等用 GT_1xx_CFGS_REG，GT9xx 用 GT_9xx_CFGS_REG；
 *       data[1..4] 为 X/Y 分辨率小端，data[6]&0x3 为中断触发方式
 */
static int gt9147_read_firmware(struct i2c_client *client, struct gt9147_dev *dev)
{
	int ret = 0, version = 0;
	u16 id = 0;
	u8 data[7]={0};
	char id_str[5];
	/* 读 PID 区：4 字节 ID 字符串 + 2 字节版本 */
	ret = gt9147_read_regs(dev, GT_PID_REG, data, 6);
	if (ret) {
		dev_err(&client->dev, "Unable to read PID.\n");
		return ret;
	}
	memcpy(id_str, data, 4);
	id_str[4] = 0; /* 转 C 字符串 */
    if (kstrtou16(id_str, 10, &id)) /* 字符串转数字，失败则兜底 0x1001 */
        id = 0x1001;
	version = get_unaligned_le16(&data[4]); /* 小端 16 位版本 */
	dev_info(&client->dev, "ID %d, version: %04x\n", id, version);
	switch (id) {    /* 不同芯片配置区地址不同，需分流 */
    case 1151:
    case 1158:
    case 5663:
    case 5688:    /* GT1xx/GT5xx 系列配置区 */
        ret = gt9147_read_regs(dev, GT_1xx_CFGS_REG, data, 7);  
		break;
    default:     /* GT9xx 系列 */
        ret = gt9147_read_regs(dev, GT_9xx_CFGS_REG, data, 7);
		break;
    }
	if (ret) {
		dev_err(&client->dev, "Unable to read Firmware.\n");
		return ret;
	}
	/* 解析分辨率与触发方式：X= data[1]+data[2]<<8，Y= data[3]+data[4]<<8 */
	dev->max_x = (data[2] << 8) + data[1];
	dev->max_y = (data[4] << 8) + data[3];
	dev->irqtype = data[6] & 0x3; /* 低 2 位为触发方式 */
	printk("X_MAX: %d, Y_MAX: %d, TRIGGER: 0x%02x", dev->max_x, dev->max_y, dev->irqtype);

	return 0;
}

/* probe：I2C 设备匹配成功时被 i2c 总线调用
 * 功能: 硬件复位→软复位→读固件→（空白则下载官方配置自愈）→注册 input→申请中断
 * 参数: client - I2C 客户端（已由内核根据设备树创建）；id - 匹配表项
 * 返回: 0 成功，负值失败
 * 被谁调用: i2c_driver 匹配 compatible="goodix,gt9147" 时内核调用
 * 顺序讲究: 先让芯片就绪并拿到正确分辨率/极性，再注册 input，最后才开中断 */
int gt9147_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
    u8 data, ret;
    gt9147.client = client;

 	/* 1，获取设备树中断与复位引脚号（interrupt-gpios/reset-gpios） */
	gt9147.irq_pin = of_get_named_gpio(client->dev.of_node, "interrupt-gpios", 0);
	gt9147.reset_pin = of_get_named_gpio(client->dev.of_node, "reset-gpios", 0);

	/* 2，硬件复位：按时序拉复位/INT，确保芯片进入可 I2C 状态 */
	ret = gt9147_ts_reset(client, &gt9147);
	if(ret < 0) {
		goto fail;
    }

    /* 3，软复位：向控制寄存器写 0x02 再清 0，确保内部状态机复位 */
    data = 0x02;
    gt9147_write_regs(&gt9147, GT_CTRL_REG, &data, 1); /* 软复位 */
    mdelay(100);
    data = 0x0;
    gt9147_write_regs(&gt9147, GT_CTRL_REG, &data, 1); /* 停止软复位 */
    mdelay(100);
	
    
    /* 4，读取固件配置，获得分辨率与中断极性 */
	ret = gt9147_read_firmware(client, &gt9147);
	if(ret != 0) {
		printk("Fail !!! check !!\r\n");
		goto fail;
    }

	/* ===== SouthBay 板适配：配置下载自愈 =====
	 * 本板 GT911 出厂配置区为全零（无出厂配置，GT9xx 零配置不扫描触摸）。
	 * 教程驱动依赖芯片出厂配置（分辨率/中断极性取自配置字节），
	 * 空白芯片必须下载官方验证配置（7寸 1024x600，正点原子
	 * bsp_gt9147.c GT9147_CFG_TBL，已打 X/Y 与下降沿极性补丁）。
	 * 流程：计算校验和（184 字节和的补码）+ fresh=1，一次性写入 186 字节，
	 *      延迟后重读固件，max_x/max_y/irqtype 随之正确，后续 input 与中断才可信。 */
	if (gt9147.max_x == 0 || gt9147.max_y == 0) {
		u8 cfg[GT9147_CFG_BODY + 2] = { 0 };
		u32 sum = 0;
		int i;

		memcpy(cfg, gt9147_cfg_tbl, sizeof(gt9147_cfg_tbl));
		for (i = 0; i < GT9147_CFG_BODY; i++)
			sum += cfg[i];
		cfg[GT9147_CFG_BODY] = (u8)((-sum) & 0xff);	/* 校验和：使 184 字节和 + 校验和 ==0 */
		cfg[GT9147_CFG_BODY + 1] = 1;			/* fresh 标志：1 表示配置更新需芯片重载 */

		if (gt9147_write_regs(&gt9147, GT_9xx_CFGS_REG, cfg,
				      GT9147_CFG_BODY + 2) < 0) {
			dev_err(&client->dev, "config download failed\n");
			goto fail;
		}
		msleep(20); /* 等芯片内部加载配置 */

		ret = gt9147_read_firmware(client, &gt9147);
		if (ret != 0) {
			dev_err(&client->dev, "re-read firmware failed\n");
			goto fail;
		}
		dev_info(&client->dev,
			 "SouthBay: official cfg downloaded, %ux%u trigger 0x%02x\n",
			 gt9147.max_x, gt9147.max_y, gt9147.irqtype);
	}

    /* 5，注册 input 设备：分配→填标识→声明能力→设绝对坐标范围→初始化 slot→注册 */
	gt9147.input = devm_input_allocate_device(&client->dev);
	if (!gt9147.input) {
		ret = -ENOMEM;
		goto fail;
	}
	gt9147.input->name = client->name; /* 供用户态识别，evtest 可见 */
	gt9147.input->id.bustype = BUS_I2C;
	gt9147.input->dev.parent = &client->dev;

	/* 声明能力：支持按键类(EV_KEY，含 BTN_TOUCH)与绝对坐标类(EV_ABS) */
	__set_bit(EV_KEY, gt9147.input->evbit);
	__set_bit(EV_ABS, gt9147.input->evbit);
	__set_bit(BTN_TOUCH, gt9147.input->keybit);

	/* 设置绝对坐标范围：ABS_X/Y 与 ABS_MT_POSITION_X/Y 均 0..max */
	input_set_abs_params(gt9147.input, ABS_X, 0, gt9147.max_x, 0, 0);
	input_set_abs_params(gt9147.input, ABS_Y, 0, gt9147.max_y, 0, 0);
	input_set_abs_params(gt9147.input, ABS_MT_POSITION_X,0, gt9147.max_x, 0, 0);
	input_set_abs_params(gt9147.input, ABS_MT_POSITION_Y,0, gt9147.max_y, 0, 0);	     
	/* 初始化多点 slot：最多 5 个触摸槽，支持多指同时 */
	ret = input_mt_init_slots(gt9147.input, MAX_SUPPORT_POINTS, 0);
	if (ret) {
		goto fail;
	}

	/* 注册 input 设备，成功后出现 /dev/input/eventX */
	ret = input_register_device(gt9147.input);
	if (ret)
		goto fail;

    /* 6，最后申请中断：此时 input 已就绪，handler 可安全上报 */
	ret = gt9147_ts_irq(client, &gt9147);
	if(ret < 0) {
		goto fail;
	}
    return 0;

fail:
	return ret;
}

/*
 * remove：I2C 设备移除或驱动卸载时调用
 * 功能: 注销 input 设备（devm 分配的 input 会随 remove 自动处理，此处显式注销以兼容非 devm 场景）
 * 参数: client - I2C 客户端
 * 返回: 0
 * 被谁调用: i2c 总线在驱动卸载或设备树节点移除时调用
 */
int gt9147_remove(struct i2c_client *client)
{
    input_unregister_device(gt9147.input);
    return 0;
}

/* 传统 I2C 设备匹配表（非设备树场景，如板文件注册） */
const struct i2c_device_id gt9147_id_table[] = {
	{ "goodix,gt9147", 0, },
    { /* sentinel */ }
};

/* 设备树匹配表：compatible="goodix,gt9147" 的节点绑定本驱动 */
const struct of_device_id gt9147_of_match_table[] = {
    {.compatible = "goodix,gt9147" },
    { /* sentinel */ }
};

/* I2C 驱动对象：probe/remove 在匹配/卸载时被调用；module_i2c_driver 宏完成注册/注销 */	
struct i2c_driver gt9147_i2c_driver = {
    .driver = {
        .name  = "gt9147",
        .owner = THIS_MODULE,
        .of_match_table = gt9147_of_match_table,
    },
    .id_table = gt9147_id_table,
    .probe  = gt9147_probe,
    .remove = gt9147_remove,
};

module_i2c_driver(gt9147_i2c_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("zuozhongkai");


