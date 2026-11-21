/*
 * 文件：uapi/led_uapi.h — LED 字符设备用户态接口头文件（给应用开发者看）
 * 所属模块：core/led — LED 驱动的用户态 API 定义
 * 作用：应用 #include 本头文件即可用 ioctl 控制板载 LED
 * 对外接口：
 *   设备节点：/dev/led （驱动加载后自动出现，ls /dev/led 验证）
 *   头文件：#include "uapi/led_uapi.h"
 *   3 条 ioctl 命令见下方宏定义，使用示例：
 *     int fd = open("/dev/led", O_RDWR);
 *     ioctl(fd, LED_IOC_ON);                 // 点亮
 *     ioctl(fd, LED_IOC_OFF);                // 熄灭
 *     int state; ioctl(fd, LED_IOC_GET, &state); // 查询，0=灭 1=亮
 *     close(fd);
 * 依赖：linux/ioctl.h（提供 _IO/_IOR 宏）
 */
#ifndef _UAPI_LED_H
#define _UAPI_LED_H

#include <linux/ioctl.h>

/* ioctl 魔数：'L'，用于内核校验命令归属（不同驱动魔数不同，避免误发） */
#define LED_IOC_MAGIC 'L'
/* 点亮 LED：无参数，内核收到后置 GPIO 有效电平 */
#define LED_IOC_ON    _IO(LED_IOC_MAGIC, 0x01)
/* 熄灭 LED：无参数，内核收到后置 GPIO 无效电平 */
#define LED_IOC_OFF   _IO(LED_IOC_MAGIC, 0x02)
/* 查询状态：内核通过 copy_to_user 回传 int（0=灭, 1=亮），用户需传 int* */
#define LED_IOC_GET   _IOR(LED_IOC_MAGIC, 0x03, int)

#endif /* _UAPI_LED_H */
