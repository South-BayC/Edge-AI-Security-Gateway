/**
 * test_imu_math.c — IMU 姿态分离/滑窗统计测试（D1）
 */
#include <stdio.h>
#include "domain/sensors/imu_math.h"

static int fails;

#define CHECK(cond, name) do { \
    if (!(cond)) { printf("FAIL: %s\n", name); fails++; } \
    else { printf("ok  : %s\n", name); } \
} while (0)

int main(void)
{
    imu_math_t m;
    int rms = -1, peak = -1;

    /* 1. 首次样本只建立重力基线，lin=0 */
    imu_math_reset(&m);
    CHECK(imu_math_linear(&m, 2048, 0, 0) == 0, "first sample sets baseline");

    /* 2. 静止多帧后 lin 接近 0 */
    int lin = 0;
    for (int i = 0; i < 200; i++)
        lin = imu_math_linear(&m, 2048, 0, 0);
    CHECK(lin <= 2, "static converges to ~0");

    /* 3. 突加冲击 → lin 明显上升 */
    lin = imu_math_linear(&m, 4096, 0, 0);
    CHECK(lin > 50, "spike raises lin");

    /* 4. 滑窗统计：全 0 时 rms/peak=0 */
    imu_math_reset(&m);
    for (int i = 0; i < IMU_MATH_WIN; i++)
        imu_math_stats(&m, 0, &rms, &peak);
    CHECK(rms == 0 && peak == 0, "stats zero");

    /* 5. 峰值与 RMS */
    imu_math_stats(&m, 100, &rms, &peak);
    CHECK(peak == 100, "peak=100");
    CHECK(rms > 0, "rms>0");

    printf("%s\n", fails ? "RESULT: FAIL" : "RESULT: PASS");
    return fails ? 1 : 0;
}
