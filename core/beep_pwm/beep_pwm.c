// SPDX-License-Identifier: GPL-2.0
/*
 * 文件：beep_pwm.c
 * 所属模块：core/beep_pwm — P4-B PWM 蜂鸣器字符设备驱动
 * 作用：让用户态通过 ioctl 控制板载无源蜂鸣器发声的频率与响度
 * 核心功能：
 *   - 4 个 ioctl：SET_FREQ(设频率 1~20000Hz)、SET_DUTY(设占空比 0~100%)、ON(启动)、OFF(静音)
 *   - 内部把频率换算为周期(period_ns=1e9/freq)，占空比换算为 duty_ns，再调 PWM 子系统
 *   - 配置与开关分离：SET_FREQ/DUTY 只改参数不直接出声，需显式 ON；duty=0 也视为静音
 *   - 互斥锁保证多进程并发 ioctl 时参数与硬件状态一致
 * 对外接口（用户态怎么用）：
 *   - 设备节点：/dev/beep_pwm0（miscdevice 动态次设备号）
 *   - 头文件：#include "uapi/beep_pwm.h"，用 ioctl(fd, BEEP_IOC_XXX, &val)
 *   - 例：int f=1000; ioctl(fd,BEEP_IOC_SET_FREQ,&f); int d=50; ioctl(fd,BEEP_IOC_SET_DUTY,&d); ioctl(fd,BEEP_IOC_ON);
 * 依赖内核子系统：
 *   - platform_driver + 设备树 (compatible="alientek,beep-pwm"，pwms 属性指向 PWM3)
 *   - miscdevice（简化字符设备注册，自动创建 /dev 节点）
 *   - PWM 子系统（pwm_config / pwm_enable / pwm_disable，适配 4.1.15 无 pwm_apply_state 的老接口）
 *   - mutex + copy_from_user（用户态数据安全拷贝）
 *
 * 特别说明（板级踩坑）：
 *   - 配置/开关分离曾导致"只设频率不响"的误判，务必先 ON；
 *   - 频率上限 20kHz 为人耳上限，超出无意义且可能超出 PWM 硬件分频范围。
 */
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/miscdevice.h>
#include <linux/pwm.h>
#include <linux/uaccess.h>
#include <linux/of.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/time.h>		/* NSEC_PER_SEC */

#include "uapi/beep_pwm.h"

/* 默认频率：1kHz，中频蜂鸣最易被人耳察觉，上电后未配置时使用 */
#define BEEP_PWM_DEFAULT_FREQ	1000

/* 设备私有数据：每个 PWM 蜂鸣器对应一个实例（本板仅一个） */
struct beep_pwm_dev {
	struct device       *dev;	/* 指向 platform_device 的 device，用于 devm 分配与打印 */
	struct pwm_device   *pwm;	/* PWM 子系统句柄，对应设备树 pwms 指向的 PWM 通道 */
	struct mutex         mutex;	/* 互斥锁：保护 freq/duty/enabled 与硬件状态的并发修改 */
	int                  freq;	/* 当前频率 Hz，范围 1~BEEP_PWM_MAX_FREQ */
	int                  duty;	/* 当前占空比 0-100，0 表示静音（即使 enabled 也不会 enable） */
	bool                 enabled;	/* 软件开关：true 表示用户已请求 ON，false 表示已 OFF */
	struct miscdevice    misc;	/* 杂项设备：负责 /dev/beep_pwm0 节点的注册与 file_operations 绑定 */
};

/*
 * 功能：把软件保存的 freq/duty/enabled 应用到硬件
 * 原理：freq→period_ns(周期纳秒)，duty→duty_ns(高电平纳秒)，调 pwm_config 设波形；
 *       再根据 enabled 与 duty 决定 pwm_enable 还是 pwm_disable
 * 参数：b - 设备私有结构（调用方必须已持有 b->mutex）
 * 返回：0 成功，负值表示 pwm_config/enable 失败
 * 调用方：所有 ioctl 分支（SET_FREQ/SET_DUTY/ON），在 mutex 保护下调用
 * 注意：pwm_config 只改波形参数，不改使能状态，使能需单独调 pwm_enable/disable
 */
static int beep_pwm_apply(struct beep_pwm_dev *b)
{
	u32 period_ns;
	u32 duty_ns;
	int ret;

	/* 频率转周期：1 秒=1e9 纳秒，除以频率得周期；用 DIV_ROUND_UP 向上取整保证不为 0 */
	period_ns = DIV_ROUND_UP(NSEC_PER_SEC, (u32)b->freq);
	/* 占空比转高电平时间：先除 100 再乘 duty，避免 32 位乘法溢出且规避 64 位除法
	 * （ARM 32 位模块内无 __aeabi_uldivmod，u64 除法会链入未导出符号导致加载失败）；
	 * 此处整除误差 <100ns，对 50us 以上周期影响 <0.2%，听感可忽略 */
	duty_ns = (period_ns / 100) * (u32)b->duty;

	/* 下发波形参数到 PWM 控制器（此时蜂鸣器是否响取决于下一步使能） */
	ret = pwm_config(b->pwm, duty_ns, period_ns);
	if (ret)
		return ret;
	/* 仅当软件开关为 ON 且占空比非 0 时才真正使能 PWM；否则保持静音 */
	if (b->enabled && b->duty > 0)
		return pwm_enable(b->pwm);
	pwm_disable(b->pwm);
	return 0;
}

/*
 * 功能：ioctl 分发——用户态控制蜂鸣器的唯一入口
 * 参数：fp - 文件句柄（private_data 指向 beep_pwm_dev）；cmd - 命令字；arg - 用户空间指针（传 int 时）或 0
 * 返回：0 成功，-EFAULT 表示拷贝失败，-EINVAL 参数越界，-ENOTTY 未知命令
 * 调用方：用户态 ioctl() 系统调用 → VFS → 本函数（已在 open 时绑定 private_data）
 */
static long beep_pwm_ioctl(struct file *fp, unsigned int cmd,
			   unsigned long arg)
{
	struct beep_pwm_dev *b = fp->private_data;
	void __user *uarg = (void __user *)arg;
	int val;
	int ret = 0;

	mutex_lock(&b->mutex);	/* 整个分发过程加锁，保证参数与硬件状态原子切换 */
	switch (cmd) {
	case BEEP_IOC_SET_FREQ:	/* 设置频率：用户传入 int 频率值 */
		if (copy_from_user(&val, uarg, sizeof(val))) {
			ret = -EFAULT;
			break;
		}
		if (val < 1 || val > BEEP_PWM_MAX_FREQ) {
			ret = -EINVAL;
			break;
		}
		b->freq = val;
		ret = beep_pwm_apply(b);
		break;
	case BEEP_IOC_SET_DUTY:	/* 设置占空比：用户传入 int 0~100 */
		if (copy_from_user(&val, uarg, sizeof(val))) {
			ret = -EFAULT;
			break;
		}
		if (val < 0 || val > 100) {
			ret = -EINVAL;
			break;
		}
		b->duty = val;
		ret = beep_pwm_apply(b);	/* 换算并下发，是否出声取决于 enabled 标志 */
		break;
	case BEEP_IOC_ON:	/* 启动：置 enabled=true 并按当前 freq/duty 使能 PWM */
		b->enabled = true;
		ret = beep_pwm_apply(b);
		break;
	case BEEP_IOC_OFF:	/* 静音：清 enabled 并直接 disable，忽略 duty */
		b->enabled = false;
		pwm_disable(b->pwm);
		break;
	default:
		ret = -ENOTTY;	/* 未识别的命令字，告诉用户“不认识” */
		break;
	}
	mutex_unlock(&b->mutex);
	return ret;
}

/*
 * 功能：open 回调——用户 open("/dev/beep_pwm0") 时执行
 * 原理：miscdevice 框架在 fp->private_data 预填了 misc 指针，用 container_of 反推 beep_pwm_dev
 * 返回：0 成功
 * 调用方：VFS，当用户 open 设备节点时
 */
static int beep_pwm_open(struct inode *inode, struct file *fp)
{
	struct beep_pwm_dev *b = container_of(fp->private_data,
					      struct beep_pwm_dev, misc);

	fp->private_data = b;	/* 后续 ioctl 通过 fp->private_data 直接拿到设备结构，无需全局查找 */
	return 0;
}

/*
 * 功能：release 回调——用户 close(fd) 时执行
 * 作用：清理 file 私有指针，避免悬空引用
 * 调用方：VFS，文件描述符关闭时
 */
static int beep_pwm_release(struct inode *inode, struct file *fp)
{
	fp->private_data = NULL;
	return 0;
}

/* 字符设备操作集：告诉内核本设备支持哪些系统调用 */
static const struct file_operations beep_pwm_fops = {
	.owner          = THIS_MODULE,
	.open           = beep_pwm_open,
	.release        = beep_pwm_release,
	.unlocked_ioctl = beep_pwm_ioctl,	/* 核心：用户态控制入口 */
	.llseek         = no_llseek,		/* 不支持 lseek，蜂鸣器无“文件位置”概念 */
};

/*
 * 功能：platform probe——设备树匹配成功时初始化硬件与字符设备
 * 调用方：platform 总线，驱动加载或设备树节点出现时
 */
static int beep_pwm_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct beep_pwm_dev *b;
	int ret;

	/* 分配零初始化的设备结构（devm_ 自动随设备卸载释放） */
	b = devm_kzalloc(dev, sizeof(*b), GFP_KERNEL);
	if (!b)
		return -ENOMEM;
	b->dev = dev;
	mutex_init(&b->mutex);

	/* 从设备树 pwms 属性获取 PWM 通道（单通道故 name 传 NULL） */
	b->pwm = devm_pwm_get(dev, NULL);
	if (IS_ERR(b->pwm))
		return PTR_ERR(b->pwm);

	/* 软件初始状态：1kHz、占空比 0（静音），硬件先配置为静音波形避免上电啸叫 */
	b->freq = BEEP_PWM_DEFAULT_FREQ;
	b->duty = 0;
	b->enabled = false;
	pwm_config(b->pwm, 0, NSEC_PER_SEC / BEEP_PWM_DEFAULT_FREQ);

	/* 注册 misc 字符设备（PWM 就绪后再暴露 /dev 节点，避免用户过早 open 失败） */
	b->misc.minor  = MISC_DYNAMIC_MINOR;	/* 动态分配次设备号 */
	b->misc.name   = "beep_pwm0";		/* 设备节点名 */
	b->misc.fops   = &beep_pwm_fops;
	b->misc.parent = dev;
	ret = misc_register(&b->misc);
	if (ret)
		return ret;

	platform_set_drvdata(pdev, b);
	return 0;
}

/*
 * 功能：platform remove——驱动卸载或设备移除时清理
 * 顺序：先摘 /dev 节点让新 open 失败 → 再关 PWM 保证静音，避免卸载后引脚悬空啸叫
 * 调用方：platform 总线
 */
static int beep_pwm_remove(struct platform_device *pdev)
{
	struct beep_pwm_dev *b = platform_get_drvdata(pdev);

	misc_deregister(&b->misc);
	pwm_disable(b->pwm);
	return 0;
}

/* 设备树匹配表：compatible="alientek,beep-pwm" 的节点会由本驱动接管 */
static const struct of_device_id beep_pwm_of_match[] = {
	{ .compatible = "alientek,beep-pwm" },
	{}
};
MODULE_DEVICE_TABLE(of, beep_pwm_of_match);	/* 供 depmod 生成别名，自动加载用 */

/* platform 驱动主体：把 probe/remove 与匹配表挂给 platform 框架 */
static struct platform_driver beep_pwm_driver = {
	.probe  = beep_pwm_probe,
	.remove = beep_pwm_remove,
	.driver = {
		.name = "alientek-beep-pwm",
		.of_match_table = beep_pwm_of_match,
	},
};
module_platform_driver(beep_pwm_driver);	/* 宏展开：注册 platform_driver 并提供 init/exit */
MODULE_LICENSE("GPL");