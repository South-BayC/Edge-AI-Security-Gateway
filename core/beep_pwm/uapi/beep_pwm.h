// SPDX-License-Identifier: GPL-2.0
/*
 * 文件：uapi/beep_pwm.h — PWM 蜂鸣器用户态接口头文件（给应用开发者看）
 * 所属模块：core/beep_pwm — 蜂鸣器驱动的用户态 API 定义
 * 作用：应用只需 #include 本头文件，即可用 ioctl 控制蜂鸣器
 * 对外接口：
 *   设备节点：/dev/beep_pwm0 （驱动加载后自动出现）
 *   头文件：#include "uapi/beep_pwm.h"
 *   4 条 ioctl 命令见下方宏定义，使用示例（伪代码）：
 *     int fd = open("/dev/beep_pwm0", O_RDWR);
 *     int freq = 2000; ioctl(fd, BEEP_IOC_SET_FREQ, &freq); // 设 2kHz
 *     int duty = 50;   ioctl(fd, BEEP_IOC_SET_DUTY, &duty); // 设 50% 响度
 *     ioctl(fd, BEEP_IOC_ON);                               // 开始响
 *     sleep(1);
 *     ioctl(fd, BEEP_IOC_OFF);                              // 静音
 *     close(fd);
 * 特别注意：
 *   - SET_FREQ / SET_DUTY 只是“配置”，不会立刻出声，必须再发 ON 才会响；
 *   - duty=0 即使 ON 也不响（视为静音），想要有声 duty 需 1~100；
 *   - 频率范围 1~20000Hz，超出返回 -EINVAL。
 * 依赖：linux/ioctl.h（提供 _IO/_IOW 宏）、linux/types.h
 */
#ifndef __BEEP_PWM_UAPI_H__
#define __BEEP_PWM_UAPI_H__

#include <linux/ioctl.h>
#include <linux/types.h>

/* ioctl 魔数：'B'，用于内核校验命令是否属于本驱动 */
#define BEEP_PWM_MAGIC		'B'
/* 频率上限：20kHz，人耳约 20kHz 封顶，再高听不见且可能超硬件分频 */
#define BEEP_PWM_MAX_FREQ	20000

/* 设置频率：用户传入 int*（Hz），内核 copy_from_user 读取 */
#define BEEP_IOC_SET_FREQ	_IOW(BEEP_PWM_MAGIC, 1, int)
/* 设置占空比：用户传入 int*（0~100），0=静音，100=常高 */
#define BEEP_IOC_SET_DUTY	_IOW(BEEP_PWM_MAGIC, 2, int)
/* 启动：按当前 freq/duty 使能 PWM（无参数） */
#define BEEP_IOC_ON		_IO(BEEP_PWM_MAGIC, 3)
/* 静音：立即关闭 PWM（无参数） */
#define BEEP_IOC_OFF		_IO(BEEP_PWM_MAGIC, 4)

#endif /* __BEEP_PWM_UAPI_H__ */
