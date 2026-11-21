/* SPDX-License-Identifier: GPL-2.0 */
/* ------------------------------------------------------------------
 * 文件: gpio_event_capture/trace/edt_capture.h
 * 模块: gpio_event 采集 tracepoint 定义（P3，内核 ftrace 观测）
 * 作用: 定义 4 个模块内 tracepoint，供 ftrace 在不改驱动逻辑的前提下
 *       零开销观测事件全路径：硬中断 → 入队 → 丢弃 → 用户态读取。
 * 核心功能:
 *   - edt_capture_irq:      中断入口（通道/电平/时间戳）
 *   - edt_capture_enqueue:  入队成功（序列号/通道/FIFO 深度）
 *   - edt_capture_drop:     丢弃（序列号/通道/FIFO 深度，含溢出与采样错误）
 *   - edt_capture_read:     用户态消费（事件数/剩余深度）
 * 对外接口（怎么用）:
 *   - 内核侧: gpio_event_capture.c 定义 CREATE_TRACE_POINTS 后 #include 本文件，
 *     调用 trace_edt_capture_*() 即埋点
 *   - 用户侧观测: mount -t tracefs tracefs /sys/kernel/tracing
 *     echo 1 > /sys/kernel/tracing/events/edt_capture/enable
 *     cat /sys/kernel/tracing/trace_pipe
 *     即可实时看到上述 4 类事件，配合统计验证丢包与时序
 * 依赖的内核机制:
 *   - tracepoint + ftrace + TRACE_EVENT 宏（内核静态探针）
 *   - TRACE_SYSTEM / TRACE_INCLUDE_PATH / define_trace.h 展开机制
 * 设计要点: 本文件会被包含两次（头定义 + define_trace.h 生成实例），
 *           故用 TRACE_HEADER_MULTI_READ 保护块 + 末尾双重 include 技巧
 * ------------------------------------------------------------------ */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM edt_capture

#if !defined(_TRACE_EDT_CAPTURE_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_EDT_CAPTURE_H

#include <linux/tracepoint.h>

/* tracepoint：IRQ 入口——硬中断刚进 handler 时打点，记录通道/电平/时间戳
 * 用法: trace_edt_capture_irq(channel, level, ts)；ftrace 中显示为 channel=... */
TRACE_EVENT(edt_capture_irq,
	TP_PROTO(int channel, int level, u64 timestamp_ns),
	TP_ARGS(channel, level, timestamp_ns),
	TP_STRUCT__entry(
		__field(int, channel)
		__field(int, level)
		__field(u64, timestamp_ns)
	),
	TP_fast_assign(
		__entry->channel = channel;
		__entry->level = level;
		__entry->timestamp_ns = timestamp_ns;
	),
	TP_printk("channel=%d level=%d ts=%llu",
		  __entry->channel, __entry->level, __entry->timestamp_ns)
);

/* tracepoint：入队成功——事件已写入 kfifo，记录序列号/通道/当前 FIFO 深度
 * 观测要点: 结合 irq 点可算中断到入队的延迟；fifo_len 可看堆积 */
TRACE_EVENT(edt_capture_enqueue,
	TP_PROTO(u32 sequence, int channel, unsigned int fifo_len),
	TP_ARGS(sequence, channel, fifo_len),
	TP_STRUCT__entry(
		__field(u32, sequence)
		__field(int, channel)
		__field(unsigned int, fifo_len)
	),
	TP_fast_assign(
		__entry->sequence = sequence;
		__entry->channel = channel;
		__entry->fifo_len = fifo_len;
	),
	TP_printk("seq=%u channel=%d fifo_len=%u",
		  __entry->sequence, __entry->channel, __entry->fifo_len)
);

/* tracepoint：丢弃——FIFO 满或采样错误导致事件未入队，但序列号已消耗（守恒）
 * 观测要点: 出现此点说明有丢包，需看统计 fifo_overflow/sample_error 区分原因 */
TRACE_EVENT(edt_capture_drop,
	TP_PROTO(u32 sequence, int channel, unsigned int fifo_len),
	TP_ARGS(sequence, channel, fifo_len),
	TP_STRUCT__entry(
		__field(u32, sequence)
		__field(int, channel)
		__field(unsigned int, fifo_len)
	),
	TP_fast_assign(
		__entry->sequence = sequence;
		__entry->channel = channel;
		__entry->fifo_len = fifo_len;
	),
	TP_printk("seq=%u channel=%d fifo_len=%u",
		  __entry->sequence, __entry->channel, __entry->fifo_len)
);

/* tracepoint：用户态读取——read() 成功消费事件，记录本次事件数与剩余 FIFO 深度
 * 观测要点: 可算用户态消费速率，判断是否跟得上中断产生速率 */
TRACE_EVENT(edt_capture_read,
	TP_PROTO(int events, unsigned int fifo_len),
	TP_ARGS(events, fifo_len),
	TP_STRUCT__entry(
		__field(int, events)
		__field(unsigned int, fifo_len)
	),
	TP_fast_assign(
		__entry->events = events;
		__entry->fifo_len = fifo_len;
	),
	TP_printk("events=%d fifo_len=%u",
		  __entry->events, __entry->fifo_len)
);

#endif /* _TRACE_EDT_CAPTURE_H */

/* 以下部分必须在保护块之外：trace 框架的“二次包含”生成技巧
 * 第一次包含生成头声明，第二次经 define_trace.h 生成探针实例；
 * TRACE_INCLUDE_PATH = trace 配合 Makefile ccflags-y += -I$(src)，
 * 使 #include "trace/edt_capture.h" 在模块目录下正确命中 */
#undef TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH trace
#undef TRACE_INCLUDE_FILE
#define TRACE_INCLUDE_FILE edt_capture
#include <trace/define_trace.h>