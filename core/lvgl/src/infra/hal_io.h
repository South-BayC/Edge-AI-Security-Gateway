/**
 * hal_io.h — 板载执行器访问（infra 层）
 *
 * 只负责“把电平写进设备节点”，不含任何告警/UI 策略：
 *   - LED：      /dev/led（自定义 ioctl LED_IOC_ON/OFF）
 *   - 蜂鸣器：   /sys/class/leds/beep/brightness（gpio-leds，内核已处理极性）
 *   - PWM 蜂鸣器：/dev/beep_pwm0（beep_pwm 驱动 ioctl）
 *
 * 依赖：仅 OS API（open/ioctl/write）。禁止包含 lvgl.h / ui/*。
 * 线程约定：只在 UI 主线程（dev_bridge_poll）或初始化路径调用。
 */
#ifndef HAL_IO_H
#define HAL_IO_H

#ifdef __cplusplus
extern "C" {
#endif

#define HAL_PWM_DEFAULT_FREQ 1000   /* beep_pwm 默认 1kHz */

/**
 * 打开全部执行器节点。单项失败只禁用该项（不影响其他外设）。
 * @return 0=至少一项可用；-1=全部不可用
 */
int hal_io_open(void);

/** 关闭全部节点（退出流程调用，可重复调用） */
void hal_io_close(void);

int hal_io_led_ok(void);
int hal_io_beep_ok(void);
int hal_io_pwm_ok(void);

/** LED 输出（内部去重：同值不重复下发） */
void hal_io_led_write(int on);
/** 当前 LED 状态：1=亮 0=灭 -1=未知 */
int hal_io_led_state(void);

/** 蜂鸣器输出（gpio-leds brightness，内部去重） */
void hal_io_beep_write(int on);
/** 当前蜂鸣器状态：1=响 0=停 -1=未知 */
int hal_io_beep_state(void);

/**
 * PWM 蜂鸣器输出。
 * @param on   1=开 0=关
 * @param freq Hz（100~20000，越界自动裁剪）
 * @param duty 占空比 1~100（默认建议 30，降低对摄像头的电源干扰）
 */
void hal_io_pwm_write(int on, int freq, int duty);
int hal_io_pwm_state(void);   /* 1=开 0=关 */
int hal_io_pwm_freq(void);    /* 当前频率，未打开过则为默认值 */

#ifdef __cplusplus
}
#endif

#endif /* HAL_IO_H */
