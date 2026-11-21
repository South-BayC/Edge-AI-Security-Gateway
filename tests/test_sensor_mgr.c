/**
 * test_sensor_mgr.c — 传感器滤波/故障升级/恢复测试（D1）
 */
#include <stdio.h>

#include "sensor_mgr.h"

static int fails;

#define CHECK(cond, name) do { \
    if (!(cond)) { printf("FAIL: %s\n", name); fails++; } \
    else { printf("ok  : %s\n", name); } \
} while (0)

int main(void)
{
    sensor_mgr_init();

    /* 1. 稳定输入 → OK */
    for (int i = 0; i < 12; i++)
        sensor_mgr_update_imu(100, 200, 300, 1000);
    CHECK(sensor_mgr_get_status(SENSOR_ID_ICM20608) == SENSOR_OK, "stable -> OK");

    /* 2. 越界连续 3 次 → OUT_OF_RANGE */
    for (int i = 0; i < 3; i++)
        sensor_mgr_update_imu(40000, 0, 0, 1000);
    CHECK(sensor_mgr_get_status(SENSOR_ID_ICM20608) == SENSOR_OUT_OF_RANGE,
          "out-of-range -> fault");

    /* 3. 恢复（大跳变会被当成 spike，需要多来几次重建基线） */
    for (int i = 0; i < 6; i++)
        sensor_mgr_update_imu(100, 200, 300, 1000);
    CHECK(sensor_mgr_get_status(SENSOR_ID_ICM20608) == SENSOR_OK, "recovered -> OK");

    /* 4. 滤波输出有效 */
    int ax = 0, ay = 0, az = 0, t = 0;
    sensor_mgr_get_imu(&ax, &ay, &az, &t);
    CHECK(ax != 0 || ay != 0 || az != 0, "filtered output non-zero");

    /* 5. invalid_cnt 清零：连续 2 次异常后成功一次，再来 1 次异常不应升级
     *    （旧实现 invalid_cnt 只增不减，这里会累计到 3 而误升级） */
    sensor_mgr_update_imu(40000, 0, 0, 1000);
    sensor_mgr_update_imu(40000, 0, 0, 1000);
    sensor_mgr_update_imu(100, 200, 300, 1000);   /* 成功样本：清零 */
    sensor_mgr_update_imu(40000, 0, 0, 1000);
    CHECK(sensor_mgr_get_status(SENSOR_ID_ICM20608) == SENSOR_OK,
          "invalid_cnt reset after success");

    printf("%s\n", fails ? "RESULT: FAIL" : "RESULT: PASS");
    return fails ? 1 : 0;
}
