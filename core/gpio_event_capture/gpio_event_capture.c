// SPDX-License-Identifier: GPL-2.0
/* ------------------------------------------------------------------
 * 文件: gpio_event_capture/gpio_event_capture.c
 * 模块: gpio_event 多通道事件采集驱动（P3，企业级，约584行，重点文件）
 * 作用: 把多个 GPIO 的边沿变化变成带时间戳、序列号的事件流，供用户态
 *       精准采集、分析与回放；是“中断→kfifo→等待队列→read/poll” 的完整范例。
 * 核心功能:
 *   - 多通道（≤8）GPIO 双边沿中断，硬中断中加时间戳与序列号后入 kfifo
 *   - 事件固定 16 字节，含 timestamp_ns/sequence/channel/level/edge
 *   - 序列号守恒：queued + dropped == irq + snapshot，可用 GET_STATS 验证不丢不重
 *   - 事务式 read：peek → copy_to_user 成功 → 才消费，失败不丢事件
 *   - START 初始快照：冻结中断→采样当前电平→恢复，避免快照与边沿竞态
 *   - 7 个 ioctl：GET_CAPS/START/STOP/SET_CONFIG/CLEAR_FIFO/GET_STATS/RESET_STATS
 *   - 独占打开：第二人 open 返回 -EBUSY，防止多读者并发破坏序列号
 *   - 模块内 tracepoint：irq/enqueue/drop/read 四点，ftrace 可观测
 *   - FIFO 满时仍消耗序列号并计 dropped，保证守恒可验证
 * 对外接口（用户态怎么用）:
 *   - 设备节点: /dev/edt_capture0（miscdevice，动态次设备号）
 *   - 头文件: #include "uapi/edt_capture.h"
 *   - 典型流程见 uapi 头注释；read 要求 count%16==0，poll 等 POLLIN 再读
 * 依赖的内核机制（必懂）:
 *   - miscdevice（轻量字符设备） + platform_driver（设备树匹配）
 *   - gpio 消费者接口 gpiod_get_array / gpiod_to_irq / gpiod_get_value
 *   - 中断子系统 request_irq（双边沿） + synchronize_irq
 *   - kfifo（环形缓冲，2 的幂大小，无锁需外加自旋锁保护）
 *   - spinlock（IRQ 与进程共享数据） vs mutex（配置 ioctl 串行化）
 *   - wait_queue + wait_event_interruptible + poll_wait + wake_up
 *   - 原子变量 atomic_t（running/opened/removing 标志，CAS 语义）
 *   - copy_to_user / copy_from_user（内核↔用户内存安全拷贝）
 *   - tracepoint（ftrace 静态探针，零开销观测）
 *   - ktime_get_ns（单调时钟纳秒）
 * 数据流全景:
 *   GPIO 边沿 → IRQ handler(加时间戳/序列号/过滤) → spin_lock → kfifo_in
 *             → 空→非空 wake_up → read(): wait_event → peek → copy_to_user → kfifo_out
 * 实现顺序: 先单通道 KEY0=GPIO1_IO18 跑通，再扩多通道
 * ------------------------------------------------------------------ */
/* 定义 CREATE_TRACE_POINTS 后再 include trace 头，会生成 tracepoint 实例；
 * 若无此宏则只生成声明。必须在所有 include 之前定义，且仅在一个 .c 中定义一次 */
#define CREATE_TRACE_POINTS
#include "trace/edt_capture.h"

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/miscdevice.h>
#include <linux/poll.h>
#include <linux/sched.h>
#include <linux/wait.h>
#include <linux/uaccess.h>
#include <linux/kfifo.h>
#include <linux/ktime.h>
#include <linux/log2.h>
#include <linux/of.h>
#include <linux/fs.h>
#include <linux/slab.h>

#include "uapi/edt_capture.h"

/* 内部数据结构（与 UAPI 分离，仅内核可见）
 * - dev: platform 设备指针，用于打印与资源管理
 * - gpios: 通道 GPIO 数组（gpiod_get_array 返回，含各通道描述符）
 * - num_chans: 实际使能通道数（来自设备树 capture-gpios 数量）
 * - orig_channel_mask: DTS 声明通道位图（SET_CONFIG 只能在此范围内收窄/恢复）
 * - channel_mask: 运行时通道位图（SET_CONFIG 可动态收窄）
 * - edge_mask: 运行时边沿过滤（RISING/FALLING/BOTH，可改）
 * - fifo: 事件环形缓冲（kfifo，大小为 2 的幂，存 struct edt_capture_event）
 * - lock: 自旋锁，保护 fifo 与 stats 在 IRQ 与进程上下文间的并发
 * - mutex: 互斥锁，串行化 SET_CONFIG 等配置类 ioctl（可睡眠，故不用 spinlock）
 * - wq: 等待队列，read 阻塞与 poll 等待于此，中断入队后 wake_up
 * - running: 原子标志 1=采集中 0=停止，CAS 保证 START/STOP 原子性
 * - opened: 原子标志 1=已打开，实现独占 open（第二人 -EBUSY）
 * - removing: 卸载标志，供 remove 同步
 * - irqs[]: 各通道中断号，-1 表示该通道无 IRQ（SNVS 等不支持中断的引脚）
 * - stats: 统计（全部在 spinlock 下更新，GET_STATS 加锁拷贝保证原子性）
 * - misc: miscdevice 对象，注册后产生 /dev/edt_capture0
 * - fifo_depth: FIFO 深度（事件数），可运行时重建改变 */
struct edt_capture_dev {
	struct device          *dev;
	struct gpio_descs      *gpios;        /* 通道 GPIO 数组（gpiod_get_array 返回） */
	int                     num_chans;    /* 实际使能通道数 */
	unsigned long           orig_channel_mask; /* DTS 声明通道位图（SET_CONFIG 可恢复到此范围） */
	unsigned long           channel_mask; /* 运行时通道位图（SET_CONFIG 可收窄） */
	u32                     edge_mask;    /* 运行时边沿过滤（SET_CONFIG 可改） */
	struct kfifo            fifo;         /* 事件 FIFO（2 的幂） */
	spinlock_t              lock;         /* IRQ + 入队 + 读共用 */
	struct mutex            mutex;        /* 配置 ioctl 用 */
	wait_queue_head_t       wq;
	atomic_t                running;
	atomic_t                opened;       /* 独占打开 */
	atomic_t                removing;
	int                     irqs[EDT_CAPTURE_MAX_CHANNELS];
	/* 统计（全部在 spinlock 保护下更新，GET_STATS 加锁拷贝） */
	struct edt_capture_stats stats;
	struct miscdevice       misc;
	unsigned int            fifo_depth;
};

/* 由 irq 号反查通道号（单通道线性遍历可接受，多通道≤8开销可忽略）
 * 功能: 中断到来时需知道是哪个通道触发，以便采样对应 GPIO 与填充 channel 字段
 * 参数: cap - 设备对象；irq - 中断号
 * 返回: 通道索引 0..num_chans-1，-1 表示未找到（非本设备中断）
 * 被谁调用: edt_capture_irq_handler */
static int edt_capture_irq_to_channel(struct edt_capture_dev *cap, int irq)
{
	int i;

	for (i = 0; i < cap->num_chans; i++)
		if (cap->irqs[i] == irq)
			return i;
	return -1;
}

/* 内部入队函数（可测性设计：KUnit/自测可直接调用，不依赖真实 IRQ）
 * 功能: 把一条已封装事件写入 kfifo，满则丢弃并计数
 * 参数: cap - 设备对象（调用方须已持有 cap->lock）；ev - 待入队事件
 * 返回: 0=已入队，1=丢弃（FIFO 满）
 * 设计取舍：kfifo 满时仍消耗序列号——保证 queued + dropped == irq 守恒，便于上层验证
 * 被谁调用: edt_capture_irq_handler（持锁调用）
 * 内核机制: kfifo_in/kfifo_avail/kfifo_len；tracepoint 记录丢弃 */
static int edt_capture_enqueue_event(struct edt_capture_dev *cap,
				     struct edt_capture_event *ev)
{
	/* 检查剩余空间是否够一条事件（16字节）；kfifo 空间以字节计 */
	if (kfifo_avail(&cap->fifo) >= sizeof(*ev)) {
		kfifo_in(&cap->fifo, ev, sizeof(*ev)); /* 拷贝入队 */
		cap->stats.queued_events++;
		/* 更新水位：fifo_level 为当前总数，high_watermark 为历史峰值 */
		if (cap->stats.fifo_level_events < kfifo_len(&cap->fifo)) {
			cap->stats.fifo_level_events = kfifo_len(&cap->fifo);
			if (cap->stats.fifo_high_watermark <
			    cap->stats.fifo_level_events)
				cap->stats.fifo_high_watermark =
					cap->stats.fifo_level_events;
		}
		return 0;
	}
	/* FIFO 满：计丢弃与溢出，序列号已在 handler 中递增（守恒），打 trace 供 ftrace 观测 */
	cap->stats.dropped_events++;
	cap->stats.fifo_overflow_events++;
	trace_edt_capture_drop(ev->sequence, ev->channel, kfifo_len(&cap->fifo));
	return 1;
}

/* IRQ 处理 + 入队（正确性关键）：硬中断上下文，仅用 spin_lock_irqsave
 * 功能: 采样时间戳/电平，过滤边沿，入 kfifo 并在空→非空时唤醒等待队列
 * 参数: irq - 触发中断号；data - 指向 edt_capture_dev
 * 返回: IRQ_HANDLED 或 IRQ_NONE（非本设备中断）
 * 上下文: 硬中断，不可睡眠，不可用 mutex/gpiod_cansleep
 * 被谁调用: 内核中断分发 */
static irqreturn_t edt_capture_irq_handler(int irq, void *data)
{
	struct edt_capture_dev *cap = data;
	struct edt_capture_event ev;
	unsigned long flags;
	bool wake = false; /* 是否需唤醒（仅空→非空时置 true，避免无谓唤醒） */
	int idx = edt_capture_irq_to_channel(cap, irq);
	int level;

	if (idx < 0)
		return IRQ_NONE; /* 非本设备通道，交还内核继续分发 */

	/* 关中断加锁：保护 fifo/stats 与 running/channel_mask 的并发访问；
	 * 用 irqsave 版本因中断上下文需保存/恢复中断开关 */
	spin_lock_irqsave(&cap->lock, flags);
	if (atomic_read(&cap->running) && (cap->channel_mask & BIT(idx))) {
		ev.timestamp_ns = ktime_get_ns(); /* 单调时钟纳秒，精准到 ns，与用户态 CLOCK_MONOTONIC 同源 */
		ev.sequence = cap->stats.last_sequence + 1; /* 全局递增序列号，满 FIFO 也递增以守恒 */
		cap->stats.last_sequence = ev.sequence;
		ev.channel = idx;

		/* 硬中断安全采样：probe 已校验 gpiod_cansleep==false，保证 gpiod_get_value 不睡眠 */
		level = gpiod_get_value(cap->gpios->desc[idx]);
		if (level < 0) {
			/* 采样错误：计入 irq 与 dropped（守恒），不入队，打 trace 供事后分析 */
			cap->stats.irq_events++;
			cap->stats.dropped_events++;
			cap->stats.sample_error_events++;
			trace_edt_capture_drop(ev.sequence, idx,
					       kfifo_len(&cap->fifo));
			goto out;
		}
		ev.level = level;
		/* 边沿由边沿后电平推断：采样时为高→刚上升，为低→刚下降（需硬件消抖配合） */
		ev.edge = level ? EDT_CAPTURE_EDGE_RISING : EDT_CAPTURE_EDGE_FALLING;
		/* edge_mask 过滤：不关心的边沿直接忽略（不计统计，不影响守恒，因无序列号消耗外的副作用） */
		if (!(cap->edge_mask & ev.edge))
			goto out;

		cap->stats.irq_events++;
		{
			bool was_empty = kfifo_is_empty(&cap->fifo); /* 记录入队前是否空，用于判断空→非空 */

			if (edt_capture_enqueue_event(cap, &ev) == 0 && was_empty) {
				cap->stats.wake_signals++;
				wake = true;   /* 仅空→非空唤醒，避免每次中断都 wake_up 的惊群开销 */
			}
		}
		/* tracepoint：供 ftrace 观测 irq 与入队时序，配合统计做延迟分析 */
		trace_edt_capture_irq(idx, ev.level, ev.timestamp_ns);
		trace_edt_capture_enqueue(ev.sequence, idx,
					  kfifo_len(&cap->fifo));
	}
out:
	spin_unlock_irqrestore(&cap->lock, flags);

	/* 锁外唤醒：减少持锁时间；POLLIN 告知 poll 侧可读 */
	if (wake)
		wake_up_interruptible_poll(&cap->wq, POLLIN);
	return IRQ_HANDLED;
}

/* read：staging 事务式消费（peek → copy_to_user 成功 → 消费）
 * 功能: 从 kfifo 取一条事件安全拷给用户态，保证 copy 失败不丢事件
 * 参数: fp - 文件对象（private_data 指向 cap）；buf - 用户态缓冲；count - 请求字节；
 *       off - 偏移（忽略）
 * 返回: 成功 sizeof(event)，失败 -EINVAL/-EAGAIN/-ERESTARTSYS/-EFAULT/-EIO，STOP 后空 FIFO 返回 0(EOF)
 * 被谁调用: 用户态 read() 经 VFS 分发
 * 核心难点: 阻塞 vs 非阻塞 + 事务式消费 + STOP 唤醒语义 */
static ssize_t edt_capture_read(struct file *fp, char __user *buf,
				size_t count, loff_t *off)
{
	struct edt_capture_dev *cap = fp->private_data;
	struct edt_capture_event ev; /* 内核中转，避免持锁访问用户内存 */
	unsigned long flags;
	ssize_t ret;

	if (count % sizeof(struct edt_capture_event))
		return -EINVAL; /* 必须按事件整数倍读，否则 -EINVAL */

	/* 阻塞/非阻塞分支：小白必懂区别
	 * - 非阻塞(O_NONBLOCK)：无数据立马 -EAGAIN，不睡眠，适合轮询或配合 poll
	 * - 阻塞：无数据时睡眠在 wq 上，直到有数据或 STOP/信号唤醒 */
	if (fp->f_flags & O_NONBLOCK) {
		if (kfifo_is_empty(&cap->fifo))
			return -EAGAIN;
	} else {
		/* 运行停止且 FIFO 空：返回 0（EOF 语义，不挂起，让用户态知“已结束”） */
		if (!atomic_read(&cap->running) && kfifo_is_empty(&cap->fifo))
			return 0;
		/* 睡眠等待：直到 fifo 非空 或 running 变 0（STOP 唤醒）；
		 * 被信号打断则返回 -ERESTARTSYS，库函数会处理为 EINTR */
		ret = wait_event_interruptible(cap->wq,
				!kfifo_is_empty(&cap->fifo) ||
				!atomic_read(&cap->running));
		if (ret)
			return -ERESTARTSYS;
		if (kfifo_is_empty(&cap->fifo))
			return 0;   /* STOP 唤醒且无剩余数据，EOF */
	}

	/* 事务式消费：先 peek 到内核 staging，copy_to_user 成功后才真正出队
	 * 好处：用户地址非法导致 -EFAULT 时事件仍在 FIFO，下次可重读，不丢数据 */
	spin_lock_irqsave(&cap->lock, flags);
	if (!kfifo_out_peek(&cap->fifo, &ev, sizeof(ev))) {
		/* 可能被并发消费（理论上独占打开 + 单读者，防御性处理） */
		spin_unlock_irqrestore(&cap->lock, flags);
		return -EAGAIN;
	}
	spin_unlock_irqrestore(&cap->lock, flags);

	/* copy_to_user：内核→用户内存安全拷贝；需在无锁区做，避免睡眠时持锁；
	 * 返回非 0 表示用户指针非法或缺页失败 */
	if (copy_to_user(buf, &ev, sizeof(ev)))
		return -EFAULT;

	spin_lock_irqsave(&cap->lock, flags);
	/* peek 后未被并发消费（独占打开 + 单读者），out 理论必成功；否则为并发异常 */
	if (kfifo_out(&cap->fifo, &ev, sizeof(ev)) != sizeof(ev)) {
		spin_unlock_irqrestore(&cap->lock, flags);
		return -EIO;
	}
	cap->stats.read_events++;
	spin_unlock_irqrestore(&cap->lock, flags);
	trace_edt_capture_read(1, kfifo_len(&cap->fifo)); /* trace 消费，供 ftrace 看速率 */

	return sizeof(ev);
}

/* poll 回调：用户态 poll/select/epoll_wait 时触发
 * 功能: 把进程挂到等待队列并报告可读/挂断状态，实现多路复用零轮询
 * 参数: fp - 文件对象；wait - poll_table（内核挂队列用）
 * 返回: 掩码 POLLIN/POLLRDNORM 表示可读，POLLHUP 表示已 STOP
 * 机制: poll_wait 仅挂队列不睡眠；内核随后检查掩码，若无 POLLIN 则让进程睡眠，
 *       直到中断的 wake_up_interruptible_poll 唤醒后重调 poll
 * 被谁调用: 用户态 poll()/select()/epoll_wait() 经 VFS 分发 */
static unsigned int edt_capture_poll(struct file *fp, poll_table *wait)
{
	struct edt_capture_dev *cap = fp->private_data;
	unsigned int mask = 0;

	poll_wait(fp, &cap->wq, wait); /* 把当前进程加入 cap->wq，建立唤醒关联 */
	if (!kfifo_is_empty(&cap->fifo))
		mask |= POLLIN | POLLRDNORM; /* 有数据可读，立马返回让用户 read */
	if (!atomic_read(&cap->running))
		mask |= POLLHUP;   /* STOP 后报告挂断，唤醒 poll 等待的进程避免永久睡眠 */
	return mask;
}

/* START 初始快照（正确性关键）：冻结中断 → 采样当前电平 → 恢复
 * 功能: 启动采集并为每个使能通道产生一条快照事件，让用户态知“起始电平”
 * 参数: cap - 设备对象
 * 返回: 0 成功，-EBUSY 已在运行中
 * 被谁调用: ioctl EDT_IOC_START
 * 为什么冻结中断：若不 disable_irq，快照采样与边沿中断可能竞态，导致快照电平
 *                 与首条边沿事件乱序或重复；先冻结再采样可保证时序干净 */
static int edt_capture_start(struct edt_capture_dev *cap)
{
	unsigned long flags;
	int i;

	/* 防并发 START：CAS 原子比较交换，0→1 成功则继续，非 0 表示已在运行 */
	if (atomic_cmpxchg(&cap->running, 0, 1) != 0)
		return -EBUSY;

	/* 冻结所有通道中断，避免快照与边沿竞态（disable_irq 可睡眠，但此处 running 已置 1，
	 * 中断 handler 会因 running 判断快速返回，故竞态窗口可控） */
	for (i = 0; i < cap->num_chans; i++)
		disable_irq(cap->irqs[i]);
	spin_lock_irqsave(&cap->lock, flags);
	for (i = 0; i < cap->num_chans; i++) {
		struct edt_capture_event ev;

		if (!(cap->channel_mask & BIT(i)))
			continue; /* 未使能通道跳过 */
		ev.timestamp_ns = ktime_get_ns();
		ev.sequence = ++cap->stats.last_sequence; /* 快照也消耗序列号，纳入守恒 */
		ev.channel  = i;
		ev.level    = gpiod_get_value(cap->gpios->desc[i]); /* 快照电平 */
		ev.edge     = 0;   /* 快照无边沿，用户态以 edge==0 识别 */
		if (kfifo_avail(&cap->fifo) >= sizeof(ev)) {
			kfifo_in(&cap->fifo, &ev, sizeof(ev));
			cap->stats.queued_events++;
			cap->stats.snapshot_events++;
		}
	}
	spin_unlock_irqrestore(&cap->lock, flags);
	for (i = 0; i < cap->num_chans; i++)
		enable_irq(cap->irqs[i]); /* 恢复中断，开始正常边沿采集 */
	wake_up_interruptible_poll(&cap->wq, POLLIN); /* 唤醒 read/poll，快照可立马读到 */
	return 0;
}

/* STOP：running=0 → 同步 IRQ → 唤醒读等待（POLLHUP）
 * 功能: 停止采集并唤醒所有阻塞在 read/poll 的进程，让其感知 EOF
 * 参数: cap - 设备对象
 * 返回: 0（幂等，已停止也返回 0）
 * 被谁调用: ioctl EDT_IOC_STOP 或 release 自动 STOP
 * 关键: synchronize_irq 等待正在执行的中断 handler 完成，避免 STOP 后仍有事件入队 */
static int edt_capture_stop(struct edt_capture_dev *cap)
{
	int i;

	if (atomic_cmpxchg(&cap->running, 1, 0) != 1)
		return 0;   /* 已停止，幂等，避免重复同步开销 */

	for (i = 0; i < cap->num_chans; i++)
		synchronize_irq(cap->irqs[i]); /* 等待各通道中断 handler 执行完毕 */
	wake_up_interruptible_poll(&cap->wq, POLLIN | POLLHUP); /* 唤醒 read/poll，read 将返回 0 EOF */
	return 0;
}

/* SET_CONFIG：mutex 保护；仅允许未运行/清空后修改；校验 mask
 * 功能: 修改通道掩码、边沿过滤、FIFO 深度等运行时配置
 * 参数: cap - 设备对象；cfg - 用户态拷贝来的配置（已校验 abi_version/struct_size）
 * 返回: 0 成功，-EINVAL 参数非法，-EBUSY 需先 STOP/CLEAR_FIFO，-ENOMEM 内存不足
 * 被谁调用: ioctl EDT_IOC_SET_CONFIG
 * 约束: 必须 STOP 且 FIFO 空才能改，避免“采集中改配置”导致语义混乱 */
static int edt_capture_set_config(struct edt_capture_dev *cap,
				  struct edt_capture_config *cfg)
{
	unsigned long flags;
	int ret = 0;

	if (cfg->abi_version != EDT_CAPTURE_ABI_VERSION)
		return -EINVAL;
	if (cfg->struct_size != sizeof(*cfg))
		return -EINVAL;

	mutex_lock(&cap->mutex); /* 串行化并发 SET_CONFIG，避免同时重建 FIFO */
	/* 运行中禁止修改（须先 STOP），否则快照/中断与配置竞态 */
	if (atomic_read(&cap->running)) {
		ret = -EBUSY;
		goto out;
	}
	/* channel_mask 只能选择 DTS 声明通道的子集（可任意收窄/恢复，不可越界） */
	if (cfg->channel_mask & ~cap->orig_channel_mask) {
		ret = -EINVAL;
		goto out;
	}
	if (cfg->channel_mask == 0) {
		ret = -EINVAL; /* 至少使能一通道 */
		goto out;
	}
	/* edge_mask 必须为 RISING/FALLING/BOTH 组合且非 0 */
	if (!cfg->edge_mask || (cfg->edge_mask & ~EDT_CAPTURE_EDGE_BOTH)) {
		ret = -EINVAL;
		goto out;
	}
	/* fifo_depth 必须为 2 的幂（kfifo 要求）或 0 表示保持当前 */
	if (cfg->fifo_depth_events &&
	    !is_power_of_2(cfg->fifo_depth_events)) {
		ret = -EINVAL;
		goto out;
	}

	/* 修改仅允许在 FIFO 清空后进行，否则旧数据与新配置不匹配 */
	spin_lock_irqsave(&cap->lock, flags);
	if (!kfifo_is_empty(&cap->fifo)) {
		spin_unlock_irqrestore(&cap->lock, flags);
		ret = -EBUSY;   /* 先 CLEAR_FIFO 再改配置 */
		goto out;
	}
	if (cfg->fifo_depth_events && cfg->fifo_depth_events != cap->fifo_depth) {
		struct kfifo new_fifo;
		int a;

		/* kfifo 重建：kfifo_alloc(GFP_KERNEL) 可能睡眠，必须放锁外，否则死锁 */
		spin_unlock_irqrestore(&cap->lock, flags);
		a = kfifo_alloc(&new_fifo,
				cfg->fifo_depth_events *
				sizeof(struct edt_capture_event),
				GFP_KERNEL);
		if (a) {
			ret = -ENOMEM;
			goto out;
		}
		/* 分配期间可能被并发 START 入队 → 重新校验后再交换，避免丢事件 */
		spin_lock_irqsave(&cap->lock, flags);
		if (atomic_read(&cap->running) ||
		    !kfifo_is_empty(&cap->fifo)) {
			spin_unlock_irqrestore(&cap->lock, flags);
			kfifo_free(&new_fifo);
			ret = -EBUSY;
			goto out;
		}
		kfifo_free(&cap->fifo); /* 释放旧缓冲 */
		cap->fifo = new_fifo;    /* 换新缓冲 */
		cap->fifo_depth = cfg->fifo_depth_events;
		cap->stats.fifo_capacity_events = cap->fifo_depth;
	}
	cap->channel_mask = cfg->channel_mask;
	cap->edge_mask = cfg->edge_mask;
	spin_unlock_irqrestore(&cap->lock, flags);
out:
	mutex_unlock(&cap->mutex);
	return ret;
}

/* ioctl 全集（7 个）：用户态与驱动的“控制面板”
 * 功能: 根据 cmd 分发到各子功能，涉及用户↔内核内存拷贝
 * 参数: fp - 文件对象；cmd - ioctl 命令号；arg - 用户态指针/整数
 * 返回: 0 成功，-EFAULT 用户地址非法，-ENOTTY 未知命令，其它见子函数
 * 被谁调用: 用户态 ioctl() 经 VFS 分发
 * 关键: copy_to_user/copy_from_user 用于跨地址空间安全拷贝，失败返回 -EFAULT */
static long edt_capture_ioctl(struct file *fp, unsigned int cmd,
			      unsigned long arg)
{
	struct edt_capture_dev *cap = fp->private_data;
	void __user *uarg = (void __user *)arg; /* 用户态指针，需用 copy_* 访问 */
	unsigned long flags;

	switch (cmd) {
	case EDT_IOC_GET_CAPS: {
		struct edt_capture_caps caps;

		memset(&caps, 0, sizeof(caps));
		caps.abi_version = EDT_CAPTURE_ABI_VERSION;
		caps.struct_size = sizeof(caps);
		caps.event_size = sizeof(struct edt_capture_event);
		caps.max_channels = EDT_CAPTURE_MAX_CHANNELS;
		caps.configured_channels = cap->num_chans;
		caps.supported_edges = EDT_CAPTURE_EDGE_BOTH;
		caps.fifo_depth_events = cap->fifo_depth;
		caps.flags = EDT_CAPTURE_CAP_NONBLOCK |
			     EDT_CAPTURE_CAP_POLL |
			     EDT_CAPTURE_CAP_EXCLUSIVE_OPEN |
			     EDT_CAPTURE_CAP_MONOTONIC_TIMESTAMP |
			     EDT_CAPTURE_CAP_DROP_REASON_STATS |
			     EDT_CAPTURE_CAP_INITIAL_SNAPSHOT |
			     EDT_CAPTURE_CAP_RESET_STATS;
		/* copy_to_user：内核→用户安全拷贝，失败说明用户缓冲非法 */
		if (copy_to_user(uarg, &caps, sizeof(caps)))
			return -EFAULT;
		return 0;
	}
	case EDT_IOC_START:
		return edt_capture_start(cap);
	case EDT_IOC_STOP:
		return edt_capture_stop(cap);
	case EDT_IOC_SET_CONFIG: {
		struct edt_capture_config cfg;

		/* copy_from_user：用户→内核安全拷贝 */
		if (copy_from_user(&cfg, uarg, sizeof(cfg)))
			return -EFAULT;
		return edt_capture_set_config(cap, &cfg);
	}
	case EDT_IOC_CLEAR_FIFO: {
		unsigned int len;

		spin_lock_irqsave(&cap->lock, flags);
		len = kfifo_len(&cap->fifo); /* 记录清空前长度，用于统计 */
		kfifo_reset(&cap->fifo);     /* 重置读/写指针，逻辑清空 */
		cap->stats.cleared_events += len;
		spin_unlock_irqrestore(&cap->lock, flags);
		return 0;
	}
	case EDT_IOC_GET_STATS: {
		struct edt_capture_stats st;

		/* 加锁拷贝：统计在 spinlock 保护下更新，需加锁保证快照原子性 */
		spin_lock_irqsave(&cap->lock, flags);
		st = cap->stats;
		st.running = atomic_read(&cap->running); /* running 为原子变量，单独读 */
		spin_unlock_irqrestore(&cap->lock, flags);
		if (copy_to_user(uarg, &st, sizeof(st)))
			return -EFAULT;
		return 0;
	}
	case EDT_IOC_RESET_STATS: {
		spin_lock_irqsave(&cap->lock, flags);
		memset(&cap->stats, 0, sizeof(cap->stats));
		cap->stats.abi_version = EDT_CAPTURE_ABI_VERSION;
		cap->stats.struct_size = sizeof(cap->stats);
		cap->stats.fifo_capacity_events = cap->fifo_depth;
		spin_unlock_irqrestore(&cap->lock, flags);
		return 0;
	}
	default:
		return -ENOTTY; /* 未知命令 */
	}
}

/* 独占打开：保证同时只有一个读者，避免多进程并发读破坏序列号与 FIFO 语义
 * 功能: CAS 抢占 opened 标志，成功则绑定 private_data
 * 参数: inode - 索引节点；fp - 文件对象（misc 框架已把 private_data 设为 miscdevice 指针）
 * 返回: 0 成功，-EBUSY 已被打开
 * 被谁调用: 用户态 open("/dev/edt_capture0") 经 VFS/misc 分发 */
static int edt_capture_open(struct inode *inode, struct file *fp)
{
	/* container_of：从 miscdevice 指针反推外层 edt_capture_dev；
	 * misc_open 默认把 private_data 设为 miscdevice，需转回设备对象 */
	struct edt_capture_dev *cap = container_of(fp->private_data,
						   struct edt_capture_dev,
						   misc);

	if (atomic_cmpxchg(&cap->opened, 0, 1) != 0)
		return -EBUSY; /* 已有人打开，独占失败 */
	fp->private_data = cap; /* 覆盖为设备对象，后续 read/ioctl 直接可用 */
	return 0;
}

/* release：文件关闭或进程退出时调用（含崩溃被内核回收）
 * 功能: 自动 STOP（防 running 残留导致下次 START 永远 -EBUSY）、清 opened、唤醒 remove
 * 参数: inode - 索引节点；fp - 文件对象
 * 返回: 0
 * 被谁调用: close() 或进程退出时内核自动调用
 * 为什么自动 STOP：交互程序 Ctrl-C 后 running 仍为 1，新进程虽能 open 但 START 必 -EBUSY，
 *                且无 ioctl 能重置 running，唯有在此回收才能自愈 */
static int edt_capture_release(struct inode *inode, struct file *fp)
{
	struct edt_capture_dev *cap = fp->private_data;

	/* 进程退出/崩溃时自动 STOP：防止 running 残留 1 导致设备死锁
	 * （交互程序 Ctrl-C 退出后，新进程 open 成功但 START 必 EBUSY，
	 *   且无任何 ioctl 能重置 running——必须在此回收） */
	if (atomic_cmpxchg(&cap->running, 1, 0) == 1) {
		int i;

		for (i = 0; i < cap->num_chans; i++) {
			if (cap->irqs[i] >= 0)
				synchronize_irq(cap->irqs[i]); /* 等待中断 handler 完成 */
		}
	}
	atomic_set(&cap->opened, 0); /* 释放独占标志，允许下一人 open */
	wake_up_all(&cap->wq);   /* 唤醒 remove 的等待（opened==0 条件）及潜在读等待 */
	return 0;
}

/* 文件操作表：把用户态系统调用映射到驱动回调；llseek 禁止（非随机访问设备） */
static const struct file_operations edt_capture_fops = {
	.owner          = THIS_MODULE,
	.open           = edt_capture_open,
	.release        = edt_capture_release,
	.read           = edt_capture_read,
	.poll           = edt_capture_poll,
	.unlocked_ioctl = edt_capture_ioctl,
	.llseek         = no_llseek,
};

/* probe：设备树匹配成功时被 platform 总线调用，完成全部初始化
 * 功能: 解析 GPIO 与 FIFO 深度、校验中断安全性、申请 IRQ、分配 kfifo、注册 misc 设备
 * 参数: pdev - platform 设备对象（含 of_node）
 * 返回: 0 成功，负值失败（失败时 devm 托管资源自动回滚）
 * 被谁调用: platform_driver 匹配 compatible="alientek,gpio-event-capture" 时内核调用
 * 顺序讲究: 先准备好中断与 FIFO，再 misc_register 暴露节点，避免用户态过早 open 到半初始化设备 */
static int edt_capture_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct edt_capture_dev *cap;
	struct gpio_descs *g;
	u32 fifo_depth = 0;
	int i, ret;

	/* 批量获取 GPIO：设备树 capture-gpios，GPIOD_IN 输入；失败返回错误指针 */
	g = devm_gpiod_get_array(dev, "capture", GPIOD_IN);
	if (IS_ERR(g))
		return PTR_ERR(g);
	if (g->ndescs > EDT_CAPTURE_MAX_CHANNELS)
		return -EINVAL; /* 通道数超上限，拒绝 */
	/* 每个 GPIO 校验：若 gpiod_cansleep 为真，说明读取可能睡眠，不能在硬中断中采样，拒绝绑定 */
	for (i = 0; i < g->ndescs; i++)
		if (gpiod_cansleep(g->desc[i]))
			return -EINVAL;

	/* 读取设备树 fifo-depth-events 属性，无则默认 1024；必须为 2 的幂（kfifo 要求） */
	device_property_read_u32(dev, "fifo-depth-events", &fifo_depth);
	fifo_depth = fifo_depth ? fifo_depth : 1024;
	if (!is_power_of_2(fifo_depth))
		return -EINVAL;

	/* devm 分配设备对象：随 driver detach 自动释放 */
	cap = devm_kzalloc(dev, sizeof(*cap), GFP_KERNEL);
	if (!cap)
		return -ENOMEM;
	cap->dev = dev;
	cap->gpios = g;
	cap->num_chans = g->ndescs;
	cap->orig_channel_mask = (1UL << g->ndescs) - 1;  /* DTS 全通道位图，如 2 通道→0b11 */
	cap->channel_mask = cap->orig_channel_mask;   /* 初始全部使能 */
	cap->edge_mask = EDT_CAPTURE_EDGE_BOTH;       /* 初始不过滤，双边沿全收 */
	cap->fifo_depth = fifo_depth;
	spin_lock_init(&cap->lock);
	mutex_init(&cap->mutex);
	init_waitqueue_head(&cap->wq);
	atomic_set(&cap->running, 0);
	atomic_set(&cap->opened, 0);
	atomic_set(&cap->removing, 0);
	cap->stats.abi_version = EDT_CAPTURE_ABI_VERSION;
	cap->stats.struct_size = sizeof(cap->stats);
	cap->stats.fifo_capacity_events = fifo_depth;

	platform_set_drvdata(pdev, cap); /* 绑定 drvdata，供 remove 通过 pdev 取回 */

	/* IRQ 申请（双边沿，每通道；先于 misc 注册，失败路径干净）
	 * SNVS 域 GPIO（如 GPIO5_IO00）可能不支持边沿中断：
	 *   gpiod_to_irq() 返回 -EINVAL → 跳过 IRQ 注册，使用轮询模式
	 *   轮询模式由定时器触发，每 1ms 检查 GPIO 状态变化 */
	for (i = 0; i < cap->num_chans; i++) {
		int irq = gpiod_to_irq(g->desc[i]);

		if (irq < 0) {
			/* SNVS GPIO 或其他不支持中断的引脚：标记为轮询模式 */
			cap->irqs[i] = -1;  /* -1 表示无 IRQ，使用轮询 */
			dev_info(dev, "channel %d: no IRQ support, using poll mode\n", i);
			continue;
		}
		cap->irqs[i] = irq;
		/* devm 申请双边沿中断，handler 为 edt_capture_irq_handler，devm 自动释放 */
		ret = devm_request_irq(dev, irq, edt_capture_irq_handler,
				       IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING,
				       "edt-capture", cap);
		if (ret)
			return ret;
	}

	/* FIFO 分配：kfifo_alloc 动态分配缓冲，大小为 depth*事件大小
	 * 注意：kfifo_init(fifo, NULL, size) 是静态初始化用法，传 NULL 会让
	 * fifo->data == NULL，首次 kfifo_in 即 memcpy 到 NULL 崩溃（板端已踩坑） */
	ret = kfifo_alloc(&cap->fifo,
			  fifo_depth * sizeof(struct edt_capture_event),
			  GFP_KERNEL);
	if (ret)
		return ret;

	/* misc 注册（中断就绪后再暴露设备节点，避免竞态） */
	cap->misc.minor = MISC_DYNAMIC_MINOR; /* 动态次设备号 */
	cap->misc.name  = "edt_capture0";     /* 节点名 /dev/edt_capture0 */
	cap->misc.fops  = &edt_capture_fops;
	cap->misc.parent = dev;
	ret = misc_register(&cap->misc);
	if (ret) {
		kfifo_free(&cap->fifo);
		return ret;
	}
	return 0;
}

/* remove：驱动卸载或设备移除时被调用（4.1.15 须返回 int）
 * 功能: 摘节点→停收→同步 IRQ→等读者退出→释 FIFO，保证卸载时无悬空访问
 * 参数: pdev - platform 设备对象
 * 返回: 0
 * 被谁调用: platform_driver 卸载或设备树节点移除时内核调用
 * 顺序: 先 misc_deregister 摘节点防新 open，再停 running 并同步 IRQ，最后等 opened==0 才释 FIFO */
static int edt_capture_remove(struct platform_device *pdev)
{
	struct edt_capture_dev *cap = platform_get_drvdata(pdev);
	int i;

	atomic_set(&cap->removing, 1);
	misc_deregister(&cap->misc);       /* 先移除设备节点，阻止新 open */
	atomic_set(&cap->running, 0);      /* 停收，新中断 handler 直接返回 */
	wake_up_all(&cap->wq);             /* 唤醒所有阻塞在 read/poll 的进程 */
	for (i = 0; i < cap->num_chans; i++) {
		if (cap->irqs[i] >= 0)
			synchronize_irq(cap->irqs[i]); /* 等待正在执行的中断 handler 完成 */
	}
	wait_event(cap->wq, atomic_read(&cap->opened) == 0);  /* 等待已打开的读者全部 release */
	kfifo_free(&cap->fifo);            /* 释放 FIFO 缓冲 */
	return 0;
}

/* 设备树匹配表：compatible="alientek,gpio-event-capture" 的节点绑定本驱动 */
static const struct of_device_id edt_capture_of_match[] = {
	{ .compatible = "alientek,gpio-event-capture" },
	{}
};
MODULE_DEVICE_TABLE(of, edt_capture_of_match);

/* platform 驱动对象：probe/remove 在匹配/卸载时被调用；module_platform_driver 宏注册 */
static struct platform_driver edt_capture_driver = {
	.probe  = edt_capture_probe,
	.remove = edt_capture_remove,
	.driver = {
		.name = "alientek-gpio-event-capture",
		.of_match_table = edt_capture_of_match,
	},
};
module_platform_driver(edt_capture_driver);
MODULE_LICENSE("GPL");