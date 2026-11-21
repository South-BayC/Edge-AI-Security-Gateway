/* ------------------------------------------------------------------
 * 文件: key_event/uapi/key_event_uapi.h
 * 模块: key_event 按键中断驱动 · 用户态接口头（UAPI）
 * 作用: 定义内核与用户态约定的唯一数据结构和 ioctl 命令，编译时
 *       同时被内核驱动和你的应用程序 include，保证两边看到的内存
 *       布局完全一致。
 * 核心功能:
 *   - struct key_event: 每次按键中断产生的事件（时间戳+键号+按下/松开）
 *   - KEY_EVENT_CLEAR: 清空驱动内未读事件的 ioctl 命令
 * 对外接口（用户态怎么用）:
 *   - 设备节点: /dev/key-event（miscdevice，驱动 probe 后自动创建）
 *   - 读事件: open("/dev/key-event", O_RDONLY) 后 read(fd, &ev, sizeof(ev))
 *            阻塞读会睡眠直到有按键，非阻塞读(O_NONBLOCK)无数据返回 -EAGAIN，
 *            也可用 poll/select 等待可读
 *   - 清空: ioctl(fd, KEY_EVENT_CLEAR)
 *   - 时间戳 timestamp_ns 是内核 ktime_get_ns() 单调时钟纳秒，可直接做
 *     抖动分析或与其它传感器对齐
 * 依赖的内核机制: UAPI 头 + ioctl 魔数约定 + packed 紧凑结构体
 * 给用户态开发者的提示:
 *   - 本头已用 __attribute__((packed)) 消除对齐空洞，sizeof==10，
 *     32位/64位程序都能直接 read，无需额外打包
 *   - key 字段当前固定为 1（KEY0），pressed 1=按下(低电平) 0=松开
 * ------------------------------------------------------------------ */
#ifndef _UAPI_KEY_EVENT_H
#define _UAPI_KEY_EVENT_H

#include <linux/ioctl.h>
#include <linux/types.h>

/* ioctl 魔数 'K'：内核用它区分不同驱动的 ioctl 命令，避免冲突 */
#define KEY_EVENT_MAGIC 'K'
/* KEY_EVENT_CLEAR：清空驱动内未读事件（无参数，_IO 表示无数据传输） */
#define KEY_EVENT_CLEAR _IO(KEY_EVENT_MAGIC, 0x01)

/* 按键事件：驱动每次中断向用户态上报一条
 *  - timestamp_ns: 单调时钟纳秒（ktime_get_ns），不受系统时间跳变影响
 *  - key: 键号（当前仅 KEY0=1）
 *  - pressed: 1=按下 0=松开（由中断时 GPIO 电平推断，低电平=按下） */
struct key_event {
    __u64 timestamp_ns;
    __u8  key;
    __u8  pressed;   /* 1=按下 0=松开 */
} __attribute__((packed));   /* 用户态/内核态共用，用 GCC 属性保证紧凑布局，消除对齐填充 */

#endif /* _UAPI_KEY_EVENT_H */