/**
 * test_sensor_calib.c — C3 温漂拟合测试（D1）
 *
 * 1) 纯函数 sensor_calib_fit 的边界与最小二乘结果；
 * 2) 通过配置文件加载温漂参数后，apply_imu 的补偿量是否正确。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config_mgr.h"
#include "sensor_calib.h"

static int fails;

#define CHECK(cond, name) do { \
    if (!(cond)) { printf("FAIL: %s\n", name); fails++; } \
    else { printf("ok  : %s\n", name); } \
} while (0)

int main(void)
{
    int k[3];

    system("rm -rf /tmp/gw-cal");
    system("mkdir -p /tmp/gw-cal");

    /* 1. 参数非法 */
    CHECK(sensor_calib_fit(NULL, NULL, 0, k) == -1, "fit invalid args -> -1");

    /* 2. 温差不足（<2℃）不更新斜率 */
    int t_small[2] = { 1000, 1100 };
    int o_small[6] = { 0, 0, 0, 10, 20, 30 };
    CHECK(sensor_calib_fit(t_small, o_small, 2, k) == -2,
          "fit insufficient spread -> -2");

    /* 3. 纯线性：10℃ 内偏差 20/-10/40 LSB
     *    → 2 / -1 / 4 LSB/℃ → k = 2000 / -1000 / 4000 */
    int t2[2] = { 0, 3268 };
    int o2[6] = { 0, 0, 0, 20, -10, 40 };
    CHECK(sensor_calib_fit(t2, o2, 2, k) == 0, "fit 2 points");
    CHECK(abs(k[0] - 2000) <= 20, "k_x ~= 2000");
    CHECK(abs(k[1] + 1000) <= 20, "k_y ~= -1000");
    CHECK(abs(k[2] - 4000) <= 40, "k_z ~= 4000");

    /* 4. 3 点带 ±1 LSB 噪声：斜率仍应接近 2.1 LSB/℃ */
    int t3[3] = { 0, 1634, 3268 };
    int o3[9] = {
        0, 0, 0,
        10, 5, 20,
        21, 9, 41,
    };
    CHECK(sensor_calib_fit(t3, o3, 3, k) == 0, "fit 3 points with noise");
    CHECK(abs(k[0] - 2100) <= 100, "noise k_x ~= 2100");
    CHECK(abs(k[2] - 4100) <= 100, "noise k_z ~= 4100");

    /* 5. 加载配置后 apply_imu：dt=3268、k=2000 → 补偿 6536 */
    const char *path = "/tmp/gw-cal/gateway.json";
    FILE *fp = fopen(path, "w");
    if (!fp) {
        printf("FAIL: open %s\n", path);
        return 1;
    }
    fputs("{\"version\":2,\"calib\":{\"valid\":1,"
          "\"imu_off\":[0,0,0],\"imu_scale\":[1000,1000,1000],"
          "\"temp_off\":0,\"temp_k\":[2000,0,0],\"temp_ref\":0}}", fp);
    fclose(fp);

    CHECK(config_init(path) == 0, "config_init for calib");
    sensor_calib_init();

    int ax = 100, ay = 0, az = 0, temp = 3268;
    sensor_calib_apply_imu(&ax, &ay, &az, &temp);
    CHECK(ax == 100 - 6536, "temp drift compensated on x");
    CHECK(ay == 0 && az == 0, "no drift on y/z when k=0");

    printf("%s\n", fails ? "RESULT: FAIL" : "RESULT: PASS");
    return fails ? 1 : 0;
}
