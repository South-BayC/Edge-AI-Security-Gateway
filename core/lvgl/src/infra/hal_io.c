/**
 * hal_io.c — 板载执行器访问实现（infra 层）
 */
#include "hal_io.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "log_svc.h"

/* ---- 驱动 UAPI（与 core/ 各驱动 uapi 保持一致，内联声明避免跨目录依赖） ---- */

/* led_uapi.h */
#define LED_IOC_MAGIC 'L'
#define LED_IOC_ON    _IO(LED_IOC_MAGIC, 0x01)
#define LED_IOC_OFF   _IO(LED_IOC_MAGIC, 0x02)

/* beep_pwm.h（PWM 蜂鸣器手动控制） */
#define BEEP_IOC_SET_FREQ _IOW('B', 1, int)
#define BEEP_IOC_SET_DUTY _IOW('B', 2, int)
#define BEEP_IOC_ON       _IO('B', 3)
#define BEEP_IOC_OFF      _IO('B', 4)

/* ---- 设备节点 ---- */
#define DEV_LED   "/dev/led"
#define DEV_BEEP  "/sys/class/leds/beep/brightness"
/*                 ↑ 出厂 dts gpio-leds 节点（label="beep", GPIO5_IO01 低有效）
 *                   内核已处理极性：写 '1' 响 / '0' 停 */
#define DEV_PWM   "/dev/beep_pwm0"   /* 外接无源蜂鸣器（PWM3） */

static int s_led_fd  = -1;
static int s_beep_fd = -1;
static int s_pwm_fd  = -1;

static int s_led_on  = -1;   /* 上次 LED 状态（-1=未知） */
static int s_beep_on = -1;   /* 上次蜂鸣器状态（-1=未知） */
static int s_pwm_on  = 0;
static int s_pwm_freq = HAL_PWM_DEFAULT_FREQ;

int hal_io_open(void)
{
    int ok = 0;

    s_led_fd = open(DEV_LED, O_RDWR);
    if (s_led_fd < 0)
        LOGW("HAL", "%s open failed: %s (LED follow disabled)", DEV_LED, strerror(errno));
    else
        ok++;

    s_beep_fd = open(DEV_BEEP, O_WRONLY);
    if (s_beep_fd < 0)
        LOGW("HAL", "%s open failed: %s (beep follow disabled)", DEV_BEEP, strerror(errno));
    else
        ok++;

    s_pwm_fd = open(DEV_PWM, O_RDWR);   /* 仅探测在位（诊断面板显示） */
    if (s_pwm_fd >= 0)
        ok++;

    s_led_on = s_beep_on = -1;
    return ok > 0 ? 0 : -1;
}

void hal_io_close(void)
{
    if (s_led_fd >= 0)  { close(s_led_fd);  s_led_fd = -1; }
    if (s_beep_fd >= 0) { close(s_beep_fd); s_beep_fd = -1; }
    if (s_pwm_fd >= 0)  { close(s_pwm_fd);  s_pwm_fd = -1; }
}

int hal_io_led_ok(void)  { return s_led_fd >= 0; }
int hal_io_beep_ok(void) { return s_beep_fd >= 0; }
int hal_io_pwm_ok(void)  { return s_pwm_fd >= 0; }

void hal_io_led_write(int on)
{
    on = on ? 1 : 0;
    if (on == s_led_on)
        return;
    if (s_led_fd >= 0) {
        if (ioctl(s_led_fd, on ? LED_IOC_ON : LED_IOC_OFF) < 0)
            LOGE("HAL", "led ioctl: %s", strerror(errno));
    }
    s_led_on = on;
}

int hal_io_led_state(void) { return s_led_on; }

void hal_io_beep_write(int on)
{
    on = on ? 1 : 0;
    if (on == s_beep_on)
        return;
    if (s_beep_fd >= 0) {
        if (lseek(s_beep_fd, 0, SEEK_SET) >= 0) {
            char c = on ? '1' : '0';
            if (write(s_beep_fd, &c, 1) < 0)
                LOGE("HAL", "beep write: %s", strerror(errno));
        }
    }
    s_beep_on = on;
}

int hal_io_beep_state(void) { return s_beep_on; }

void hal_io_pwm_write(int on, int freq, int duty)
{
    if (freq < 100) freq = 100;
    if (freq > 20000) freq = 20000;
    if (duty < 1) duty = 1;
    if (duty > 100) duty = 100;

    if (s_pwm_fd >= 0) {
        if (on) {
            int f = freq, d = duty;
            ioctl(s_pwm_fd, BEEP_IOC_SET_FREQ, &f);
            ioctl(s_pwm_fd, BEEP_IOC_SET_DUTY, &d);
            ioctl(s_pwm_fd, BEEP_IOC_ON);
        } else {
            ioctl(s_pwm_fd, BEEP_IOC_OFF);
        }
    }
    s_pwm_on = on ? 1 : 0;
    s_pwm_freq = freq;
}

int hal_io_pwm_state(void) { return s_pwm_on; }
int hal_io_pwm_freq(void)  { return s_pwm_freq; }
