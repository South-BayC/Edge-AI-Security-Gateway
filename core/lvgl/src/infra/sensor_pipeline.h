/**
 * sensor_pipeline.h — 传感器采集流水线（infra 层）
 *
 * 职责：IIO/KEY 采集线程 + 校准 + 滤波 + 双阈值判定 + 诊断快照。
 * 通过 take_xxx 接口把事件交给 dev_bridge；不依赖 UI/LVGL。
 *
 * 线程约定：take_xxx / get_xxx 可在主线程调用；set_xxx 可在主线程调用（内部加锁）。
 */
#ifndef SENSOR_PIPELINE_H
#define SENSOR_PIPELINE_H

#include "../config_types.h"
#include "../dev_bridge.h"   /* struct dev_diag */

#ifdef __cplusplus
extern "C" {
#endif

/** 启动采集线程（KEY/AP3216C/ICM20608 + IIO buffer）；返回 0=成功 */
int  sensor_pipeline_start(void);
void sensor_pipeline_stop(void);

/** 阈值配置（dev_bridge 启动/变更时同步过来） */
void sensor_pipeline_set_config(const struct dev_config *cfg);
const struct dev_config *sensor_pipeline_get_config(void);

/** AP3216C 校准（PS/IR 本底、ALS 比例） */
void sensor_pipeline_set_ap_calib(int ps_off, int ir_off, int als_scale_x1000);
void sensor_pipeline_get_ap_calib(int *ps_off, int *ir_off, int *als_scale_x1000);

/** 采集使能开关 */
void sensor_pipeline_set_als_en(int en);
void sensor_pipeline_set_imu_en(int en);
int  sensor_pipeline_get_als_en(void);
int  sensor_pipeline_get_imu_en(void);

/* ---- 事件消费（读取即清零） ---- */
int  sensor_pipeline_take_key_toggle(void);
int  sensor_pipeline_take_warn(int zone);
int  sensor_pipeline_take_warn_pending(int zone);
int  sensor_pipeline_take_calib_done(void);

/** 诊断快照（线程安全） */
void sensor_pipeline_get_diag(struct dev_diag *out);

/** IMU buffer 通路是否可用（1=可用，采集线程走 buffer；0=回退 sysfs） */
int  sensor_pipeline_imu_buffer_ok(void);

#ifdef __cplusplus
}
#endif

#endif /* SENSOR_PIPELINE_H */
