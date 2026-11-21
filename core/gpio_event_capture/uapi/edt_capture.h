/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/* ------------------------------------------------------------------
 * 文件: gpio_event_capture/uapi/edt_capture.h
 * 模块: gpio_event 多通道事件采集 UAPI（P3，手册 5.4.2 完整版）
 * 作用: 内核与用户态的“契约头”——两边 include 同一份，保证结构体
 *       大小、ioctl 编号、ABI 版本完全一致。
 * 核心功能: 定义事件、能力、配置、统计四类结构体及 7 个 ioctl 命令
 * 对外接口（你作为用户态开发者怎么用）:
 *   - 设备节点: /dev/edt_capture0（miscdevice，动态次设备号，probe 后出现）
 *   - 头文件: #include "edt_capture.h" 即可拿到所有类型和 ioctl 号
 *   - 典型流程:
 *       fd = open("/dev/edt_capture0", O_RDWR);          // 独占打开，第二人 -EBUSY
 *       ioctl(fd, EDT_IOC_GET_CAPS, &caps);              // 查能力与 FIFO 深度
 *       cfg.abi_version=2; cfg.channel_mask=0x1; ...;
 *       ioctl(fd, EDT_IOC_SET_CONFIG, &cfg);             // 未运行+空 FIFO 时可改配置
 *       ioctl(fd, EDT_IOC_START);                        // 开始采集并产生初始快照
 *       read(fd, &ev, sizeof(ev));  // 或 poll() 等可读再读
 *       ioctl(fd, EDT_IOC_GET_STATS, &st);               // 看统计守恒 queued+dropped==irq
 *       ioctl(fd, EDT_IOC_STOP); ioctl(fd, EDT_IOC_CLEAR_FIFO);
 *   - 事件固定 16 字节，时间戳为 CLOCK_MONOTONIC 单调时钟，不受 date 跳变影响
 * 依赖的内核机制: UAPI 约定 + ioctl 魔数 'E' + miscdevice + kfifo 事件流
 * 给你的提示:
 *   - 先 GET_CAPS 确认 abi_version==2 再 SET_CONFIG，避免版本不匹配 -EINVAL
 *   - read 要求 count % 16 ==0，否则 -EINVAL；建议一次读一条（16字节）
 *   - FIFO 深度为 2 的幂，默认 1024，满时新事件丢弃但序列号仍递增（可验证）
 * ------------------------------------------------------------------ */
#ifndef _UAPI_EDT_CAPTURE_H
#define _UAPI_EDT_CAPTURE_H

#include <linux/ioctl.h>
#include <linux/types.h>

/* ABI 版本：用户态与内核须一致；不匹配时 SET_CONFIG 返回 -EINVAL，GET_CAPS 可查内核版本 */
#define EDT_CAPTURE_ABI_VERSION  2U
/* 最大通道数：硬件与软件上限，设备树声明数不能超过此值 */
#define EDT_CAPTURE_MAX_CHANNELS 8U

/* 边沿类型位掩码：用于 edge_mask 过滤与 ev.edge 标识（位组合） */
#define EDT_CAPTURE_EDGE_RISING   (1U << 0) /* 上升沿：0→1 */
#define EDT_CAPTURE_EDGE_FALLING  (1U << 1) /* 下降沿：1→0 */
#define EDT_CAPTURE_EDGE_BOTH     (EDT_CAPTURE_EDGE_RISING | EDT_CAPTURE_EDGE_FALLING) /* 双边沿 */

/* 能力位（caps.flags）：你在 GET_CAPS 后检查这些位，决定可用特性 */
#define EDT_CAPTURE_CAP_NONBLOCK           (1U << 0) /* 支持 O_NONBLOCK 非阻塞读 */
#define EDT_CAPTURE_CAP_POLL               (1U << 1) /* 支持 poll/select */
#define EDT_CAPTURE_CAP_EXCLUSIVE_OPEN     (1U << 2) /* 独占打开，第二人 open 返回 -EBUSY */
#define EDT_CAPTURE_CAP_MONOTONIC_TIMESTAMP (1U << 3) /* 时间戳为单调时钟 */
#define EDT_CAPTURE_CAP_DROP_REASON_STATS  (1U << 4) /* 统计区分溢出/采样错误等丢弃原因 */
#define EDT_CAPTURE_CAP_INITIAL_SNAPSHOT   (1U << 5) /* START 时产生初始电平快照 */
#define EDT_CAPTURE_CAP_RESET_STATS        (1U << 6) /* 支持 RESET_STATS 清零统计 */

/* 事件记录：固定 16 字节，每次 GPIO 边沿或快照产生一条，用户态 read() 的最小单位
 * 你用 read(fd, &ev, 16) 即可拿到一条，sequence 递增可用于检测丢包 */
struct edt_capture_event {
	__u64 timestamp_ns;   /* 单调时钟纳秒（ktime_get_ns），与 CLOCK_MONOTONIC 同源 */
	__u32 sequence;       /* 全局递增序列号（乱序/丢包证据，满 FIFO 时也递增以守恒） */
	__u16 channel;        /* 通道号 0-7，对应设备树 capture-gpios 的索引 */
	__u8  level;          /* 当前电平 0/1（中断后采样值） */
	__u8  edge;           /* 边沿类型 0=快照无边沿 1=rising 2=falling */
};

/* 能力查询结果：48 字节，GET_CAPS 返回；教你如何正确使用驱动 */
struct edt_capture_caps {
	__u32 abi_version;          /* 驱动 ABI 版本，应等于 EDT_CAPTURE_ABI_VERSION */
	__u32 struct_size;          /* 本结构体大小，校验用 */
	__u32 event_size;           /* 事件大小（固定 16） */
	__u32 max_channels;         /* 硬件最大通道数（8） */
	__u32 configured_channels;  /* 设备树实际配置通道数 */
	__u32 supported_edges;      /* 支持的边沿掩码（通常 BOTH） */
	__u32 fifo_depth_events;    /* FIFO 深度（事件数，2 的幂） */
	__u32 flags;                /* 能力位组合（CAP_*） */
	__u64 reserved[2];          /* 保留，置 0 */
};

/* 运行时配置：32 字节，SET_CONFIG 输入；仅在未运行且 FIFO 空时可改 */
struct edt_capture_config {
	__u32 abi_version;          /* 须填 EDT_CAPTURE_ABI_VERSION，否则 -EINVAL */
	__u32 struct_size;          /* 须填 sizeof(struct edt_capture_config)，防大小不匹配 */
	__u32 channel_mask;         /* 通道位图：bit n=1 使能通道 n，只能选 DTS 子集 */
	__u32 edge_mask;            /* 边沿过滤：RISING/FALLING/BOTH */
	__u32 fifo_depth_events;    /* FIFO 深度（0=保持当前，否则须为 2 的幂） */
	__u32 flags;                /* 保留标志 */
	__u32 reserved[2];          /* 保留 */
};

/* 统计：104 字节，GET_STATS 返回；用于验证守恒 queued+dropped==irq+snapshot */
struct edt_capture_stats {
	__u32 abi_version;
	__u32 struct_size;
	__u32 running;              /* 1=采集中 0=已停止 */
	__u32 fifo_level_events;    /* 当前 FIFO 事件数 */
	__u32 fifo_capacity_events; /* FIFO 容量 */
	__u32 fifo_high_watermark;  /* 峰值水位（历史最大 fifo_level） */
	__u32 last_sequence;        /* 最后分配的序列号 */
	__u32 reserved0;
	__u64 irq_events;           /* 进入 IRQ 次数（running 且通道使能才计） */
	__u64 queued_events;        /* 成功入队（边沿 + 初始快照） */
	__u64 read_events;          /* 用户态已读取数 */
	__u64 dropped_events;       /* 丢弃总数（溢出 + 采样错误） */
	__u64 wake_signals;         /* 空→非空唤醒次数（wake_up 次数） */
	__u64 fifo_overflow_events; /* FIFO 满丢弃数 */
	__u64 sample_error_events;  /* GPIO 采样失败数 */
	__u64 snapshot_events;      /* START 快照数 */
	__u64 cleared_events;       /* CLEAR_FIFO 清空的事件字节数/事件数 */
};

/* ioctl 命令集（魔数 'E'）：用户态用 ioctl(fd, CMD, &arg) 调用
 * _IOR=内核→用户读，_IOW=用户→内核写，_IO=无参 */
#define EDT_IOC_MAGIC 'E'
#define EDT_IOC_GET_CAPS    _IOR(EDT_IOC_MAGIC, 0x00, struct edt_capture_caps) /* 查能力 */
#define EDT_IOC_START       _IO(EDT_IOC_MAGIC, 0x01)                          /* 开始采集+快照 */
#define EDT_IOC_STOP        _IO(EDT_IOC_MAGIC, 0x02)                          /* 停止采集 */
#define EDT_IOC_SET_CONFIG  _IOW(EDT_IOC_MAGIC, 0x03, struct edt_capture_config) /* 改配置 */
#define EDT_IOC_CLEAR_FIFO  _IO(EDT_IOC_MAGIC, 0x04)                          /* 清空 FIFO */
#define EDT_IOC_GET_STATS   _IOR(EDT_IOC_MAGIC, 0x05, struct edt_capture_stats)  /* 查统计 */
#define EDT_IOC_RESET_STATS _IO(EDT_IOC_MAGIC, 0x06)                          /* 清零统计 */

/* 编译期校验：若结构体大小被意外改动（如对齐、字段增删），此处编译失败
 * 强制用户态与内核态看到的内存布局完全一致，避免跨编译器/平台错位 */
typedef char edt_capture_event_assert[
	(sizeof(struct edt_capture_event) == 16) ? 1 : -1];
typedef char edt_capture_caps_assert[
	(sizeof(struct edt_capture_caps) == 48) ? 1 : -1];
typedef char edt_capture_config_assert[
	(sizeof(struct edt_capture_config) == 32) ? 1 : -1];
typedef char edt_capture_stats_assert[
	(sizeof(struct edt_capture_stats) == 104) ? 1 : -1];

#endif /* _UAPI_EDT_CAPTURE_H */
