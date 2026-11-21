/**
 * sensor_calib.h — 传感器校准（T3-C / C3 温漂拟合）
 *
 * 当前覆盖 ICM20608 加速度零偏/比例 + 温度偏置 + 温漂斜率：
 *   - 上电静止零偏采集（GATEWAY_CALIB_ONCE=1，默认 2s）；
 *   - 手动设置 offset/scale（串口/后续 UI 入口）；
 *   - 参数持久化到 gateway.json 的 "calib" 节；
 *   - 温漂为“纯自动拟合”：每次零偏采集记录一个温度点，累计 ≥2 个不同
 *     温度点（温差 ≥2℃）后，用最小二乘拟合每轴斜率，之后按
 *     ax -= (T - T_ref) * k 自动补偿；未拟合前不补偿。
 *   - 采集线程读取本地缓存，避免每帧访问配置锁。
 *
 * 六面校准/温补实验方法见 docs/verification-guide.md 第 4 章。
 */
#ifndef SENSOR_CALIB_H
#define SENSOR_CALIB_H

#ifdef __cplusplus
extern "C" {
#endif

/** 从持久化配置加载校准参数（config_init 之后调用） */
void sensor_calib_init(void);

/**
 * 应用校准到 IMU 原始值（原地修改）。
 * 未校准时不做任何修改。
 */
void sensor_calib_apply_imu(int *ax, int *ay, int *az, int *temp_raw);

/**
 * 开始零偏采集（设备保持静止）。
 * @param duration_ms 采集时长，建议 2000；最小 500
 * @return 0=已启动
 */
int sensor_calib_start_zero_bias(int duration_ms);

/** 是否正在采集零偏 */
int sensor_calib_is_collecting(void);

/**
 * 采集线程每帧调用（传原始值，未应用校准）。
 * @return 1=本次采集完成；0=进行中/未启动；-1=失败
 */
int sensor_calib_poll(int ax, int ay, int az, int temp_raw);

/**
 * 手动设置校准参数并持久化。
 * @param scale_x1000 比例 ×1000（1000=1.0）
 */
void sensor_calib_set(int off_x, int off_y, int off_z,
                      int scale_x1000, int scale_y1000, int scale_z1000,
                      int temp_off);

/** 读取当前校准参数 */
void sensor_calib_get(int *off_x, int *off_y, int *off_z,
                      int *scale_x1000, int *scale_y1000, int *scale_z1000,
                      int *temp_off);

/** 校准是否有效 */
int sensor_calib_valid(void);

/**
 * C3 温漂拟合（纯函数，便于单元测试）。
 * 对 n 个 (温度, 零偏) 点做每轴最小二乘线性拟合。
 *
 * @param temps 温度点（原始值，326.8 LSB/℃）
 * @param offs  零偏点，扁平布局 offs[i*3 + axis]
 * @param n     点数（≥2）
 * @param k_out 输出斜率 LSB/℃ ×1000（3 轴）
 * @return 0=成功；-1=参数非法；-2=温差不足（<2℃），不更新斜率
 */
int sensor_calib_fit(const int *temps, const int *offs, int n, int k_out[3]);

/** 读取当前温漂参数（斜率 LSB/℃×1000 与参考温度） */
void sensor_calib_get_drift(int k_out[3], int *ref_temp);

/** 设置温度零点校正（原始值偏移，正值=读数偏高时下调） */
void sensor_calib_set_temp_off(int raw);

#ifdef __cplusplus
}
#endif

#endif /* SENSOR_CALIB_H */
