/**
 * dev_bridge.h — 外设信号桥接层
 *
 * 把板载驱动（LED/蜂鸣器/KEY0/AP3216C/ICM20608）接入 LVGL UI：
 *   采集线程：阻塞 poll /dev/key-event + 周期读传感器 → 共享状态（互斥保护）
 *   LVGL 定时器：主线程消费共享状态 → ui_events_* 更新 UI；
 *                LED 跟随布防状态、蜂鸣器跟随告警状态（变化时才 ioctl）
 *
 * 线程安全约定：
 *   - ui_events_* 只能在 LVGL 主线程调用（本层由 lv_timer 回调保证）
 *   - 采集线程只写共享状态，不触碰任何 LVGL 对象
 *   - 任一设备节点打开失败仅禁用该外设并打印警告，不影响其余功能
 */
#ifndef DEV_BRIDGE_H
#define DEV_BRIDGE_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 启动外设桥接（在 ui_home_create 之后、主循环之前调用一次）
 * 内部创建采集线程 + LVGL 消费定时器；重复调用无副作用。
 */
void dev_bridge_start(void);
/** 停止采集/IMU 线程并 join（退出流程调用） */
void dev_bridge_stop(void);
/** 主线程轮询（UI 每 200ms 调用）：消费共享状态、驱动告警/声光 */
void dev_bridge_poll(void);

/**
 * 模块诊断信息快照（供 UI 检测面板显示）
 */
struct dev_diag {
    /* 设备在位（节点成功打开） */
    int led_ok, beep_ok, pwm_ok;
    int key_ok, als_ok, imu_ok;
    /* 输出当前状态：1=开/响 */
    int led_on, beep_on;
    /* PWM 输出状态与频率 Hz */
    int pwm_on, pwm_freq;
    /* KEY0 当前电平：1=按下 */
    int key_pressed;
    /* AP3216C 最新原始值（ir 10bit / als 16bit / ps 10bit 越大越近） */
    int als_ir, als_als, als_ps;
    /* ICM20608 最新原始值（±16G 量程）与温度（℃×10） */
    int imu_ax, imu_ay, imu_az;
    int imu_temp_x10;
    /* C2：合加速度瞬时值 + 滑窗统计（×100，1g≈100） */
    int imu_mag_x100, imu_rms_x100, imu_peak_x100;
    int imu_lin_x100;   /* C3：线性加速度（去重力）×100 */
    /* 采集使能（测试面板手动开关） */
    int als_en, imu_en;
};

/**
 * 获取模块诊断快照（线程安全，可在 LVGL 定时器中调用）
 * @param out 输出结构体，不允许 NULL
 */
void dev_bridge_get_diag(struct dev_diag *out);

/* 预警触发源 / 数据通道 / 阈值配置类型（config_types.h） */
#include "config_types.h"

/**
 * 读取预警阈值配置
 */
void dev_bridge_get_config(struct dev_config *out);

/* AP3216C 校准（ALS 比例 ×1000，PS/IR 环境本底偏移） */
void dev_bridge_set_ap_calib(int ps_off, int ir_off, int als_scale_x1000);
void dev_bridge_get_ap_calib(int *ps_off, int *ir_off, int *als_scale_x1000);

/** 告警冷却剩余秒数（0=不在冷却） */
int dev_bridge_get_cooldown_remain(int zone);

/**
 * 设置预警阈值配置（采集线程下一轮生效）
 */
void dev_bridge_set_config(const struct dev_config *cfg);

/* ---- 测试面板手动控制（与自动跟随逻辑共享缓存，互不打架） ---- */
void dev_bridge_set_beep(int on);         /* 手动开关板载蜂鸣器 */
void dev_bridge_set_led(int on);          /* 手动开关板载 LED（告警时自动让位） */
void dev_bridge_set_pwm(int on, int freq); /* 手动开关 PWM 蜂鸣器并设频率 */
void dev_bridge_set_als_en(int en);       /* AP3216C 采集使能 */
void dev_bridge_set_imu_en(int en);       /* ICM20608 采集使能 */
int  dev_bridge_get_als_en(void);
int  dev_bridge_get_imu_en(void);
void dev_bridge_set_beep_mute(int mute);  /* 告警静音挂起（消警自动解除） */

#ifdef __cplusplus
}
#endif

#endif /* DEV_BRIDGE_H */
