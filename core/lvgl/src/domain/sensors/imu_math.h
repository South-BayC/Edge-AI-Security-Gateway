/**
 * imu_math.h — IMU 姿态分离与滑窗统计（纯算法，无硬件/UI 依赖）
 *
 * 输入为校准后的三轴原始值；输出：
 *   - 线性加速度（去重力）lin_x100；
 *   - 32 点滑窗 RMS / 峰值（对 lin 统计）。
 */
#ifndef IMU_MATH_H
#define IMU_MATH_H

#ifdef __cplusplus
extern "C" {
#endif

#define IMU_MATH_WIN 32

typedef struct {
    /* 重力低通估计 */
    float grav[3];
    int   grav_init;
    /* 滑窗（lin） */
    int   win[IMU_MATH_WIN];
    int   n;
    int   idx;
    long long sq;
} imu_math_t;

void imu_math_reset(imu_math_t *m);

/** 计算去重力线性加速度（×100，单位 g×100） */
int imu_math_linear(imu_math_t *m, int ax, int ay, int az);

/**
 * 推入一个 lin 样本并统计。
 * @param rms  输出 RMS（×100，可为 NULL）
 * @param peak 输出峰值（×100，可为 NULL）
 */
void imu_math_stats(imu_math_t *m, int lin_x100, int *rms, int *peak);

#ifdef __cplusplus
}
#endif

#endif /* IMU_MATH_H */
