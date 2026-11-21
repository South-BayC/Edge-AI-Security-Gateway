// SPDX-License-Identifier: GPL-2.0
/* ------------------------------------------------------------------
 * 文件: key_event/key_event.c
 * 模块: key_event 按键中断驱动（P2，单通道 KEY0）
 * 作用: 以字符设备形式把按键中断事件暴露给用户态，演示三种经典
 *       读取模型，是学习“中断→等待队列→read/poll”最简范例。
 * 核心功能:
 *   - GPIO 双边沿中断采样，按下/松开各产生一条带时间戳的事件
 *   - 阻塞 read：无事件时进程睡眠，直到中断唤醒
 *   - 非阻塞 read(O_NONBLOCK)：无事件立即返回 -EAGAIN
 *   - poll/select：配合等待队列实现多路复用
 *   - ioctl KEY_EVENT_CLEAR：清空未读事件
 * 对外接口（用户态怎么用）:
 *   - 设备节点: /dev/key-event（miscdevice，动态次设备号）
 *   - 读事件: read(fd, &ev, sizeof(struct key_event))，ev 定义见 uapi
 *   - 非阻塞: open 时加 O_NONBLOCK，read 无数据返回 -EAGAIN 而非睡眠
 *   - 多路复用: poll(fd, ...) 等 POLLIN 再读，避免忙轮询
 * 依赖的内核机制:
 *   - miscdevice（轻量字符设备注册，自动创建 /dev 节点）
 *   - gpio 消费者接口 gpiod_get / gpiod_to_irq / gpiod_get_value
 *   - 中断子系统 request_irq + wait_queue（等待队列）
 *   - file_operations: open/read/poll/unlocked_ioctl
 *   - 原子变量 + spinlock（中断与进程上下文共享数据保护）
 * 设计取舍: 单事件槽（非 kfifo），适合低频按键；高频场景见 P3 的 kfifo 方案
 * ------------------------------------------------------------------ */
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/miscdevice.h>
#include <linux/poll.h>
#include <linux/sched.h>
#include <linux/wait.h>
#include <linux/uaccess.h>
#include <linux/ioctl.h>
#include <linux/fs.h>
#include <linux/of.h>
#include <linux/ktime.h>

#include "uapi/key_event_uapi.h"

/* 驱动私有数据（全局单例，P2 仅支持单按键）
 *  - gpio: GPIO 描述符（gpiod 句柄）
 *  - irq: 中断号（gpiod_to_irq 转换）
 *  - last_event: 单事件槽，存放最近一次中断的事件（P3 用 kfifo 可存多条）
 *  - event_ready: 原子标志 1=有未读事件 0=已消费，供 read/poll 判断
 *  - lock: 自旋锁，保护 last_event 在中断与进程上下文间的并发访问
 *  - wq: 等待队列，阻塞读进程在此睡眠，中断到来时 wake_up 唤醒
 *  - misc: miscdevice 对象，注册后产生 /dev/key-event 节点 */
struct key_dev {
    struct gpio_desc *gpio;
    int irq;
    /* 事件存储：P2 用简单的单事件槽 + 计数（P3 升级为 kfifo） */
    struct key_event last_event;
    atomic_t event_ready;      /* 1=有未读事件 */
    spinlock_t lock;
    wait_queue_head_t wq;
    struct miscdevice misc;
};

/* 全局设备对象：P2 单通道，无需动态分配；多通道场景见 P3 的 edt_capture_dev */
static struct key_dev g_key;

/* 中断回调：GPIO 边沿触发时由内核中断子系统调用
 * 功能: 采样电平与时间戳，填充事件槽并唤醒等待队列上的读进程
 * 参数: irq - 触发的中断号（单按键未细分）；data - 指向 struct key_dev
 * 返回: IRQ_HANDLED（已处理）
 * 上下文: 硬中断上下文，不可睡眠，只能用 spin_lock_irqsave
 * 被谁调用: 内核中断分发（request_irq 注册） */
static irqreturn_t key_irq_handler(int irq, void *data)
{
    struct key_dev *k = data;
    int level = gpiod_get_value(k->gpio); /* 读取当前电平：0=低(按下) 1=高(松开) */
    unsigned long flags;

    /* 硬中断直接入队（操作简单），用 spin_lock_irqsave 关中断加锁，
     * 防止与 read() 中 spin_lock_irqsave 并发写 last_event */
    spin_lock_irqsave(&k->lock, flags);
    k->last_event.timestamp_ns = ktime_get_ns(); /* 单调时钟纳秒，精准记录按键时刻 */
    k->last_event.key = 1;                    /* KEY0 固定键号 */
    k->last_event.pressed = (level == 0);     /* 按下为低电平，故 level==0 判按下 */
    atomic_set(&k->event_ready, 1);          /* 标记有未读事件，供 read/poll 检测 */
    spin_unlock_irqrestore(&k->lock, flags);

    /* 唤醒等待队列上睡眠的进程（阻塞 read 或 poll_wait 的进程）
     * wake_up_interruptible 仅唤醒可中断睡眠的进程，与 wait_event_interruptible 配对 */
    wake_up_interruptible(&k->wq);
    return IRQ_HANDLED;
}

/* read 回调：用户态 read(fd, buf, len) 时触发
 * 功能: 按阻塞/非阻塞两种模式返回一条按键事件
 * 参数: file - 文件对象（private_data 指向 key_dev）；buf - 用户态缓冲区；
 *       count - 请求字节数；off - 文件偏移（本驱动忽略）
 * 返回: 成功返回 sizeof(key_event)，失败返回 -EINVAL/-EAGAIN/-ERESTARTSYS/-EFAULT
 * 被谁调用: 用户态 read() 系统调用经 VFS 分发 */
static ssize_t key_read(struct file *file, char __user *buf, size_t count, loff_t *off)
{
    struct key_dev *k = file->private_data;
    struct key_event ev; /* 内核中转区：先拷到此，再 copy_to_user，避免持锁期间访问用户内存 */
    unsigned long flags;

    if (count < sizeof(struct key_event))
        return -EINVAL; /* 缓冲区太小，装不下一条事件 */

    /* 阻塞 vs 非阻塞：核心区别在于“无数据时是睡眠等待还是立即返回”
     *  - 非阻塞(O_NONBLOCK)：无事件直接 -EAGAIN，让应用可忙轮询或配合 poll
     *  - 阻塞：调用 wait_event_interruptible 让进程在 wq 上睡眠，直到
     *    event_ready 变 1（中断唤醒）或被信号打断（返回 -ERESTARTSYS） */
    if (file->f_flags & O_NONBLOCK) {
        if (!atomic_read(&k->event_ready))
            return -EAGAIN;
    } else {
        if (wait_event_interruptible(k->wq, atomic_read(&k->event_ready)))
            return -ERESTARTSYS; /* 被信号打断，告知用户态重启或处理信号 */
    }

    /* 加锁拷贝事件并清标志：保证中断并发写入时数据原子性；
     * 拷到局部 ev 后立即清 event_ready，后续 copy_to_user 失败也不回滚（单槽语义） */
    spin_lock_irqsave(&k->lock, flags);
    memcpy(&ev, &k->last_event, sizeof(ev));
    atomic_set(&k->event_ready, 0);
    spin_unlock_irqrestore(&k->lock, flags);

    /* copy_to_user：从内核 ev 拷到用户 buf；返回非 0 表示用户地址非法或缺页失败 */
    if (copy_to_user(buf, &ev, sizeof(ev)))
        return -EFAULT;
    return sizeof(ev);
}

/* poll 回调：用户态 poll/select/epoll 时触发
 * 功能: 把当前进程挂到等待队列，并报告是否可读
 * 参数: file - 文件对象；wait - poll_table（内核用于挂队列）
 * 返回: 掩码 POLLIN 表示可读，0 表示暂不可读
 * 机制: poll_wait 把进程加入 wq；返回掩码告知内核是否立即可读，
 *       若为 0，内核会让进程睡眠，直到 key_irq_handler 的 wake_up 唤醒后重调 poll
 * 被谁调用: 用户态 poll()/select()/epoll_wait() 经 VFS 分发 */
static unsigned int key_poll(struct file *file, poll_table *wait)
{
    struct key_dev *k = file->private_data;

    /* poll_wait：把当前进程加入等待队列，但不睡眠；仅建立唤醒关联 */
    poll_wait(file, &k->wq, wait);
    /* 若已有未读事件，报告 POLLIN 可读；否则 0，让内核安排睡眠 */
    return atomic_read(&k->event_ready) ? POLLIN : 0;
}

/* ioctl 回调：用户态 ioctl(fd, cmd) 时触发
 * 功能: 处理带外控制命令（清空事件等）
 * 参数: file - 文件对象；cmd - 命令号；arg - 参数（本驱动未用）
 * 返回: 0 成功，-ENOTTY 表示不支持的命令
 * 被谁调用: 用户态 ioctl() 系统调用 */
static long key_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    struct key_dev *k = file->private_data;

    switch (cmd) {
    case KEY_EVENT_CLEAR: /* 清空未读事件：丢弃 last_event，供应用主动复位 */
        atomic_set(&k->event_ready, 0);
        return 0;
    default:
        return -ENOTTY; /* 未知命令 */
    }
}

/* open 回调：用户态 open("/dev/key-event") 时触发
 * 功能: 绑定文件对象到全局设备，准备后续 read/poll/ioctl
 * 参数: inode - 索引节点；file - 文件对象
 * 返回: 0 成功
 * 细节: misc 框架默认把 file->private_data 设为 miscdevice 指针，
 *       这里覆盖为 &g_key，让 read/poll 直接拿到设备对象
 * 被谁调用: 用户态 open() 经 VFS/mics 框架分发 */
static int key_open(struct inode *inode, struct file *file)
{
    /* 显式挂接私有数据（misc_open 默认放 miscdevice 指针，这里直接挂设备） */
    file->private_data = &g_key;
    return 0;
}

/* 文件操作表：把用户态系统调用映射到驱动回调 */
static const struct file_operations key_fops = {
    .owner          = THIS_MODULE,
    .open           = key_open,
    .read           = key_read,
    .poll           = key_poll,
    .unlocked_ioctl = key_ioctl,
};

/* probe：设备树匹配成功时被 platform 总线调用
 * 功能: 初始化 GPIO、中断、等待队列并注册 misc 设备
 * 参数: pdev - platform 设备对象（携带设备树节点）
 * 返回: 0 成功，负值失败
 * 被谁调用: platform_driver 匹配 compatible="alientek,key-event" 时内核调用 */
static int key_probe(struct platform_device *pdev)
{
    struct device *dev = &pdev->dev;
    int ret;

    /* gpiod 获取：设备树属性 "key-gpios"，GPIOD_IN 输入；devm 托管自动释放 */
    g_key.gpio = devm_gpiod_get(dev, "key", GPIOD_IN);
    if (IS_ERR(g_key.gpio))
        return PTR_ERR(g_key.gpio);

    /* GPIO 转中断号：失败返回负错误码（如 -EINVAL 表示不支持中断） */
    g_key.irq = gpiod_to_irq(g_key.gpio);
    if (g_key.irq < 0)
        return g_key.irq;

    /* 申请双边沿中断：按下(下降沿)和释放(上升沿)都触发 key_irq_handler
     * devm 托管，驱动卸载时自动 free；IRQF_TRIGGER_* 指定触发方式 */
    ret = devm_request_irq(dev, g_key.irq, key_irq_handler,
                           IRQF_TRIGGER_FALLING | IRQF_TRIGGER_RISING,
                           "key-event", &g_key);
    if (ret)
        return ret;

    /* 初始化同步原语：原子标志清零、自旋锁、等待队列；三者配合实现中断→进程通信 */
    atomic_set(&g_key.event_ready, 0);
    spin_lock_init(&g_key.lock);
    init_waitqueue_head(&g_key.wq);

    /* 注册 misc 设备：动态次设备号，节点名 /dev/key-event，父设备为 platform dev
     * 成功后用户态即可 open/read/poll */
    g_key.misc.minor = MISC_DYNAMIC_MINOR;
    g_key.misc.name  = "key-event";
    g_key.misc.fops  = &key_fops;
    g_key.misc.parent = dev;
    ret = misc_register(&g_key.misc);
    if (ret)
        return ret;
    return 0;
}

/* remove：驱动卸载或设备移除时被调用
 * 功能: 注销 misc 设备，避免 misc_list 残留野指针指向已卸载模块内存
 * 参数: pdev - platform 设备对象
 * 返回: 0
 * 被谁调用: platform_driver 卸载或设备树节点移除时内核调用 */
static int key_remove(struct platform_device *pdev)
{
    misc_deregister(&g_key.misc);
    return 0;
}

static const struct of_device_id key_of_match[] = {
    { .compatible = "alientek,key-event" },
    {}
};
MODULE_DEVICE_TABLE(of, key_of_match);

static struct platform_driver key_driver = {
    .probe  = key_probe,
    .remove = key_remove,
    .driver = {
        .name = "alientek-key-event",
        .of_match_table = key_of_match,
    },
};
module_platform_driver(key_driver);
MODULE_LICENSE("GPL");