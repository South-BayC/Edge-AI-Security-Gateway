// SPDX-License-Identifier: GPL-2.0
/*
 * 文件：led_drv.c
 * 所属模块：core/led — P1 LED 字符设备驱动（最简平台驱动示例）
 * 作用：控制板载 LED 灯的亮/灭，供用户态与网关应用做告警指示
 * 核心功能：
 *   - 通过 GPIO 描述符(gpiod) 驱动 LED，支持设备树配置极性(GPIO_ACTIVE_LOW/HIGH)
 *   - 3 个 ioctl：ON(点亮)、OFF(熄灭)、GET(查询当前状态)
 *   - 使用 miscdevice 动态次设备号，自动创建 /dev/led
 *   - 单全局实例 g_led，结构极简，适合小白入门理解“字符设备→GPIO”的完整链路
 * 对外接口（用户态怎么用）：
 *   - 设备节点：/dev/led
 *   - 头文件：#include "uapi/led_uapi.h"
 *   - 例：int fd=open("/dev/led",O_RDWR); ioctl(fd, LED_IOC_ON); int s; ioctl(fd, LED_IOC_GET, &s);
 * 依赖内核子系统：
 *   - platform_driver + 设备树 (compatible="alientek,led"，属性 led-gpios)
 *   - gpio 消费者接口 gpiod (devm_gpiod_get / gpiod_set_value)
 *   - miscdevice（简化字符设备注册）
 *   - ioctl + copy_to_user（用户态数据回传）
 */
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/gpio/consumer.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>
#include <linux/of.h>
#include <linux/ioctl.h>
#include <linux/fs.h>

/* ioctl 魔数与命令字（与 uapi/led_uapi.h 保持一致，此处为内核侧副本） */
#define LED_IOC_MAGIC 'L'					/* 魔数 'L'，用于校验命令归属 */
#define LED_IOC_ON    _IO(LED_IOC_MAGIC, 0x01)		/* 点亮 LED（无参数） */
#define LED_IOC_OFF   _IO(LED_IOC_MAGIC, 0x02)		/* 熄灭 LED（无参数） */
#define LED_IOC_GET   _IOR(LED_IOC_MAGIC, 0x03, int)	/* 读取状态：内核 copy_to_user 回传 int(0/1) */

/* 设备私有数据：本驱动仅支持单 LED，故用全局单例而非动态分配 */
struct led_dev {
    struct gpio_desc *gpio;	/* GPIO 描述符：指向设备树 led-gpios 对应的引脚，已由 gpiod 封装极性 */
    int state;			/* 软件记录的当前状态：0=灭, 1=亮（与 gpiod 值同步，便于 GET 快速返回） */
    struct miscdevice misc;	/* 杂项设备：负责 /dev/led 节点与 file_operations 绑定 */
};

/* 全局单例：probe 时填充，ioctl/remove 直接使用 */
static struct led_dev g_led;

/*
 * 功能：ioctl 分发——用户态控制 LED 的唯一入口
 * 参数：f - 文件句柄（本驱动未用 private_data，直接用全局 g_led）；cmd - 命令字；arg - 用户空间指针（GET 时为 int*）
 * 返回：0 成功，-EFAULT 拷贝失败，-ENOTTY 未知命令
 * 调用方：用户态 ioctl() → VFS → 本函数
 * 关键逻辑：
 *   - ON/OFF：直接调 gpiod_set_value 驱动引脚电平（极性已由设备树 GPIO_ACTIVE_* 处理，1=亮）
 *   - GET：用 copy_to_user 把内核态的 g_led.state 安全拷贝回用户空间，不可直接解引用用户指针
 */
static long led_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
    switch (cmd) {
    case LED_IOC_ON:
        gpiod_set_value(g_led.gpio, 1);	/* 置引脚有效电平，点亮 */
        g_led.state = 1;		/* 同步软件状态 */
        return 0;
    case LED_IOC_OFF:
        gpiod_set_value(g_led.gpio, 0);	/* 置引脚无效电平，熄灭 */
        g_led.state = 0;
        return 0;
    case LED_IOC_GET:
        /* 把当前状态拷贝给用户；copy_to_user 返回非 0 表示用户指针非法或缺页失败 */
        return copy_to_user((void __user *)arg, &g_led.state, sizeof(int)) ? -EFAULT : 0;
    default:
        return -ENOTTY;	/* 未识别命令，告知用户“不认识此 ioctl” */
    }
}

/* 字符设备操作集：本驱动只提供 ioctl，不支持 read/write */
static const struct file_operations led_fops = {
    .owner          = THIS_MODULE,
    .unlocked_ioctl = led_ioctl,	/* 核心：用户控制入口 */
};

/*
 * 功能：platform probe——设备树匹配成功时初始化 GPIO 与字符设备
 * 参数：pdev - platform 设备实例
 * 返回：0 成功，非 0 失败
 * 调用方：platform 总线
 */
static int led_probe(struct platform_device *pdev)
{
    struct device *dev = &pdev->dev;
    int ret;

    /* 获取 GPIO 描述符：从设备树 "led-gpios" 属性解析引脚，初始方向输出低电平（灭）
     * 极性（ACTIVE_LOW/HIGH）由设备树描述，gpiod_set_value(1) 始终表示“亮” */
    g_led.gpio = devm_gpiod_get(dev, "led", GPIOD_OUT_LOW);
    if (IS_ERR(g_led.gpio))
        return PTR_ERR(g_led.gpio);

    /* 注册 misc 字符设备，成功后出现 /dev/led */
    g_led.misc.minor = MISC_DYNAMIC_MINOR;	/* 动态分配次设备号，避免与系统固定设备冲突 */
    g_led.misc.name  = "led";			/* 设备节点名 */
    g_led.misc.fops  = &led_fops;
    g_led.misc.parent = dev;			/* 关联父设备，便于 /sys 拓扑 */
    ret = misc_register(&g_led.misc);
    if (ret)
        return ret;
    return 0;
}

/*
 * 功能：platform remove——驱动卸载时注销字符设备
 * 细节：必须 misc_deregister，否则 misc 链表残留悬空指针，下次 open 可能踩已卸载内存
 * 调用方：platform 总线
 */
static int led_remove(struct platform_device *pdev)
{
    misc_deregister(&g_led.misc);
    return 0;
}

/* 设备树匹配表：compatible="alientek,led" 的节点由本驱动接管 */
static const struct of_device_id led_of_match[] = {
    { .compatible = "alientek,led" },
    {}
};
MODULE_DEVICE_TABLE(of, led_of_match);	/* 供 depmod 生成别名 */

/* platform 驱动主体 */
static struct platform_driver led_driver = {
    .probe  = led_probe,
    .remove = led_remove,
    .driver = {
        .name = "alientek-led",
        .of_match_table = led_of_match,
    },
};
module_platform_driver(led_driver);	/* 注册 platform_driver */
MODULE_LICENSE("GPL");
