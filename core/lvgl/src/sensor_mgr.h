/**
 * sensor_mgr.h — 传感器数据质量管理（T3-A）
 *
 * 职责：
 *   - 对 AP3216C（ALS/PS/IR）和 ICM20608（AX/AY/AZ/TEMP）的原始值做：
 *       范围检查 → 变化率检查 → 3σ 离群剔除 → EMA + 中值滤波；
 *   - 连续无效/通信失败/数据陈旧时产生一次“状态变化”事件，供 UI/日志上报；
 *   - 不依赖 LVGL，可独立测试；线程安全（采集线程写、UI 线程读）。
 *
 * 设计约定：
 *   - 单次异常只返回状态、不立刻上报；连续 3 次才升级为故障；
 *   - 有效样本会清除计数并恢复 OK；
 *   - 故障/恢复都通过 sensor_mgr_take_fault() 取走（读后清除），避免日志刷屏。
 *
 * T3-A 只做应用层；T3-B 再给 icm20608 驱动加 IIO buffer+trigger，
 * 把 IMU 采样从 10Hz 提升到 100~200Hz。
 */
#ifndef SENSOR_MGR_H
#define SENSOR_MGR_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SENSOR_OK = 0,
    SENSOR_STALE,          /* 超过预期周期没有新数据 */
    SENSOR_OUT_OF_RANGE,   /* 超出物理范围 */
    SENSOR_SPIKE,          /* 相邻样本变化率过大 */
    SENSOR_OUTLIER,        /* 3σ 离群 */
    SENSOR_COMM_ERR,       /* IIO 读取连续失败 */
    SENSOR_SATURATED       /* 接近量程上限（预留） */
} sensor_status_t;

#define SENSOR_ID_AP3216C  0
#define SENSOR_ID_ICM20608 1
#define SENSOR_ID_MAX      2

/** 初始化/复位内部状态（上电或校准后调用） */
void sensor_mgr_init(void);
void sensor_mgr_reset(void);

/**
 * 更新一帧 AP3216C 原始值（ALS 16bit / PS 10bit / IR 10bit）。
 * 返回本次处理结果；异常样本不会污染滤波值。
 */
sensor_status_t sensor_mgr_update_ap(int als, int ps, int ir);

/**
 * 更新一帧 ICM20608 原始值（加速度 ±16G，温度 raw）。
 */
sensor_status_t sensor_mgr_update_imu(int ax, int ay, int az, int temp_raw);

/**
 * 标记一次通信失败（IIO read 返回失败时调用）。
 * 连续 3 次失败会升级为 SENSOR_COMM_ERR。
 */
void sensor_mgr_mark_fail(int sensor_id);

/** 读取滤波后的 AP3216C 值（任意输出指针可为 NULL） */
void sensor_mgr_get_ap(int *als, int *ps, int *ir);

/** 读取滤波后的 ICM20608 值（temp_raw 仍为原始温度刻度） */
void sensor_mgr_get_imu(int *ax, int *ay, int *az, int *temp_raw);

/** 当前传感器状态（取各通道最严重者） */
sensor_status_t sensor_mgr_get_status(int sensor_id);

/**
 * 检查数据陈旧（由 UI 200ms 定时器周期调用）。
 * 超过 stale 时间无新样本 → SENSOR_STALE，并产生一次状态事件。
 * @return 本次是否有状态变化
 */
int sensor_mgr_check_stale(void);

/**
 * 取一次“状态变化”事件（读后清除）。
 * @param sensor_id 输出传感器 ID
 * @param status    输出新状态（SENSOR_OK 表示恢复）
 * @param name      输出传感器名（"AP3216C"/"ICM20608"）
 * @return 1=有事件；0=无
 */
int sensor_mgr_take_fault(int *sensor_id, sensor_status_t *status,
                          char *name, int name_n);

/** 状态枚举转字符串（日志用） */
const char *sensor_status_name(sensor_status_t st);

#ifdef __cplusplus
}
#endif

#endif /* SENSOR_MGR_H */
