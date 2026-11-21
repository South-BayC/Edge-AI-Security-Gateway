// SPDX-License-Identifier: GPL-2.0
/* ------------------------------------------------------------------
 * 文件: key_input/key_input.c
 * 模块: key_input · input 子系统按键驱动（P6，手册 5.15.1）
 * 作用: 把 KEY0（GPIO1_IO18）包装成标准 Linux input 设备，让应用
 *       通过通用 input 事件流读取按键，无需自定义协议。
 * 核心功能:
 *   - GPIO 双边沿中断 → 采样逻辑电平 → input_report_key + input_sync
 *   - 按键码由设备树 linux,code 指定（默认 KEY_ARMED=227）
 *   - 与 P2 key_event 对比：P2 自研字符设备需专用工具链，P6 走标准
 *     input 生态，evtest / Qt / libevdev / LVGL evdev 直接可用
 * 对外接口（用户态怎么用）:
 *   - 设备节点: /dev/input/eventX（input 子系统自动创建，X 由内核分配）
 *   - 读取: evtest /dev/input/eventX 或 read(eventX) 解析 struct input_event
 *   - 区分设备: 用 ioctl(EVIOCGNAME) 或 /proc/bus/input/devices 找
 *     "imx6ull-gpio-keys"
 * 依赖的内核机制:
 *   - platform_driver + of_match_table（设备树 compatible 匹配）
 *   - gpio 消费者接口 gpiod_get / gpiod_to_irq / gpiod_get_value
 *   - input 子系统 devm_input_allocate_device / input_register_device
 *   - 中断子系统 devm_request_irq（双边沿触发）
 *   - devm 资源托管（gpio/irq/input 随 driver detach 自动释放，无需 remove）
 * 设计要点:
 *   1. 全 devm 托管，probe 失败路径干净，无需手写 remove 回滚
 *   2. gpiod_get_value 返回逻辑电平（已按 GPIO_ACTIVE_LOW 翻转），
 *      按下即 1，可直接传给 input_report_key，避免二次取反
 *   3. 按键码可配置，增强复用性
 * 对比文档: docs/driver-architecture-decision.md
 * ------------------------------------------------------------------ */
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/input.h>
#include <linux/of.h>

/* 4.1.15 内核头文件无 KEY_ARMED 宏（新内核 input-event-codes.h 才有），
 * 此处兜底定义，保证代码在老内核可编译；实际键值以设备树 linux,code 为准 */
#ifndef KEY_ARMED
#define KEY_ARMED	227	/* 兜底键码：与设备树 linux,code=<227> 保持一致，便于上层识别 */
#endif

/* 驱动私有数据：一个物理按键对应一份
 *  - gpio: GPIO 描述符（gpiod 句柄，含 active-low 信息）
 *  - irq:  gpiod_to_irq 转换得到的中断号
 *  - input: input 子系统设备对象（上报按键事件的载体）
 *  - key_code: 上报给 input 子系统的按键码（来自设备树，可配） */
struct key_input_dev {
	struct gpio_desc *gpio;
	int irq;
	struct input_dev *input;
	int key_code;		/* 设备树 linux,code 指定，默认 KEY_ARMED=227 */
};

/* 中断回调：按键电平变化时被内核中断子系统调用
 * 功能: 采样当前电平并通过 input 子系统上报按键按下/释放
 * 参数: irq - 中断号（未使用，单按键场景）; data - 指向 struct key_input_dev
 * 返回: IRQ_HANDLED（表示已处理）
 * 上下文: 硬中断上下文，不可睡眠，只能做轻量操作
 * 被谁调用: 内核中断分发（devm_request_irq 注册的 handler） */
static irqreturn_t key_input_handler(int irq, void *data)
{
	struct key_input_dev *k = data;
	/* 采样逻辑电平：gpiod 已按设备树 GPIO_ACTIVE_LOW 自动翻转，
	 * 物理低(按下) → 逻辑 1，物理高(松开) → 逻辑 0，直接可用，无需再取反 */
	int pressed = gpiod_get_value(k->gpio);

	/* 向 input 子系统报告按键状态：pressed=1 按下，0 释放
	 * input_report_key 会把事件缓存在 input_dev 的事件队列中 */
	input_report_key(k->input, k->key_code, pressed);
	/* 同步：告诉 input 子系统“这一批事件发完了”，触发一次向用户态的完整事件帧
	 * 4.1.15 已统一为 input_sync()，旧 API input_sync_key 已移除 */
	input_sync(k->input);
	return IRQ_HANDLED;
}

/* probe：设备树匹配成功时被 platform 总线调用，完成硬件与 input 设备初始化
 * 功能: 解析设备树、申请 GPIO/IRQ、创建并注册 input 设备
 * 参数: pdev - platform 设备对象（含 of_node 指向设备树节点）
 * 返回: 0 成功，负值失败（失败时 devm 托管资源自动回滚）
 * 被谁调用: platform_driver 匹配 compatible="alientek,key-input" 时由内核调用 */
static int key_input_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct key_input_dev *k;
	int ret;

	/* devm 分配私有数据：随设备 detach 自动释放，无需手动 kfree */
	k = devm_kzalloc(dev, sizeof(*k), GFP_KERNEL);
	if (!k)
		return -ENOMEM;
	platform_set_drvdata(pdev, k);

	/* 读取设备树 linux,code 属性作为按键码；缺省则用 KEY_ARMED(227)，
	 * 这样同一驱动可复用于不同按键，只需改设备树 */
	ret = of_property_read_u32(dev->of_node, "linux,code", &k->key_code);
	if (ret) {
		dev_info(dev, "linux,code 未指定，默认 KEY_ARMED(227)\n");
		k->key_code = KEY_ARMED;
	}

	/* 获取 GPIO 描述符：设备树 key-gpios，配置为输入；失败返回错误指针 */
	k->gpio = devm_gpiod_get(dev, "key", GPIOD_IN);
	if (IS_ERR(k->gpio))
		return PTR_ERR(k->gpio);

	/* GPIO 转中断号：内核 pinctrl/gpio 子系统返回对应 irq 号，<0 表示不支持中断 */
	k->irq = gpiod_to_irq(k->gpio);
	if (k->irq < 0)
		return k->irq;

	/* 分配 input 设备对象：devm 托管，driver 卸载时自动注销并释放 */
	k->input = devm_input_allocate_device(dev);
	if (!k->input)
		return -ENOMEM;

	/* 填充 input 设备标识：name 供用户态识别（/proc/bus/input/devices 可见），
	 * phys/bustype/vendor/product/version 构成设备唯一信息 */
	k->input->name = "imx6ull-gpio-keys";
	k->input->phys = "gpio-keys/input0";
	k->input->id.bustype = BUS_HOST;
	k->input->id.vendor  = 0x0001;
	k->input->id.product = 0x0001;
	k->input->id.version = 0x0100;

	/* 声明设备能力：支持 EV_KEY 类事件，并声明具体支持的按键码
	 * __set_bit 标记大类，input_set_capability 标记具体码，二者配合供
	 * 用户态通过 EVIOCGBIT 查询设备能上报哪些事件 */
	__set_bit(EV_KEY, k->input->evbit);
	input_set_capability(k->input, EV_KEY, k->key_code);

	/* 申请双边沿中断：按下(下降沿)和释放(上升沿)都会触发 key_input_handler
	 * devm_request_irq 自动管理生命周期，detach 时自动 free */
	ret = devm_request_irq(dev, k->irq, key_input_handler,
			       IRQF_TRIGGER_FALLING | IRQF_TRIGGER_RISING,
			       "key-input", k);
	if (ret)
		return ret;

	/* 注册 input 设备：成功后内核创建 /dev/input/eventX，用户态即可 open/read */
	ret = input_register_device(k->input);
	if (ret)
		return ret;

	dev_info(dev, "key-input 注册成功：code=%d(%s) irq=%d\n",
		 k->key_code, "KEY_ARMED", k->irq);
	return 0;
}

/* 设备树匹配表：compatible="alientek,key-input" 的节点会绑定到本驱动 */
static const struct of_device_id key_input_of_match[] = {
	{ .compatible = "alientek,key-input" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, key_input_of_match);

/* platform 驱动对象：probe 在匹配时被调用；全 devm 资源无需 remove */
static struct platform_driver key_input_driver = {
	.probe	= key_input_probe,
	.driver	= {
		.name		= "alientek-key-input",
		.of_match_table	= key_input_of_match,
	},
};
module_platform_driver(key_input_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("SouthBay");
MODULE_DESCRIPTION("KEY0 input subsystem driver (P6, manual 5.15.1)");