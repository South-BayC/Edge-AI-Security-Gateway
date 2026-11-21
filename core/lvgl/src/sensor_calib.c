/**
 * sensor_calib.c — 传感器校准实现（T3-C / C3 温漂拟合）
 *
 * 校准模型：
 *   ax' = ((ax_raw - (T - T_ref) * k_x / 1000) - off_x) * scale_x / 1000
 *   ay' / az' 同理
 *   temp' = temp_raw - temp_off
 *   其中 k 单位 LSB/℃×1000，T 为温度原始值；k=0 表示尚未拟合，不补偿。
 *
 * 零偏采集：
 *   设备静止 duration_ms，累加原始样本求均值作为 offset；
 *   同时把 (温度, 三轴均值) 记为一个温度点，累计 ≥2 个不同温度点后
 *   最小二乘拟合温漂斜率；完成后写入 gateway.json 并立即 flush。
 *
 * 线程模型：
 *   - 采集线程调用 poll()/apply_imu()；
 *   - 主线程/命令调用 set()；
 *   - 本地缓存由 g_lock 保护，避免每帧读配置。
 */
#include "sensor_calib.h"

#include "config_mgr.h"
#include "log_svc.h"

#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ICM20608：326.8 LSB/℃；温差阈值 2℃ */
#define TEMP_LSB_PER_C   326.8
#define TEMP_FIT_MIN_RAW 654       /* 2℃（326.8 LSB/℃），温差不足不拟合 */
#define TEMP_SAME_RAW    327       /* ±1℃ 视为同一温度点 */
#define TEMP_POINTS_MAX  4
#define TEMP_K_LIMIT     20000     /* 斜率上限 ±20 LSB/℃×1000 */

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static int g_valid = 0;
static int g_off[3] = { 0, 0, 0 };
static int g_scale[3] = { 1000, 1000, 1000 };
static int g_temp_off = 0;
static int g_temp_k[3] = { 0, 0, 0 };
static int g_temp_ref = 0;
static int g_pt_n = 0;
static int g_pt_temp[TEMP_POINTS_MAX];
static int g_pt_off[3][TEMP_POINTS_MAX];

static int g_collecting = 0;
static uint64_t g_start_ms = 0;
static uint32_t g_duration_ms = 0;
static int g_n = 0;
static long long g_sum[4] = { 0, 0, 0, 0 };

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000L);
}

/* 从配置加载到本地缓存（调用方持锁） */
static void load_from_config_locked(void)
{
    const struct app_config *c = config_get();
    g_valid = c->calib_valid;
    memcpy(g_off, c->imu_off, sizeof(g_off));
    memcpy(g_scale, c->imu_scale, sizeof(g_scale));
    g_temp_off = c->temp_off;
    memcpy(g_temp_k, c->temp_k, sizeof(g_temp_k));
    g_temp_ref = c->temp_ref;
    g_pt_n = c->calib_pt_n;
    memcpy(g_pt_temp, c->calib_pt_temp, sizeof(g_pt_temp));
    memcpy(g_pt_off, c->calib_pt_off, sizeof(g_pt_off));
}

void sensor_calib_init(void)
{
    int valid, off[3], scale[3], temp_off, k[3], ref, pts;

    pthread_mutex_lock(&g_lock);
    load_from_config_locked();
    g_collecting = 0;
    valid = g_valid;
    memcpy(off, g_off, sizeof(off));
    memcpy(scale, g_scale, sizeof(scale));
    temp_off = g_temp_off;
    memcpy(k, g_temp_k, sizeof(k));
    ref = g_temp_ref;
    pts = g_pt_n;
    pthread_mutex_unlock(&g_lock);

    LOGI("CALIB", "loaded valid=%d off=[%d,%d,%d] scale=[%d,%d,%d] temp_off=%d",
         valid, off[0], off[1], off[2],
         scale[0], scale[1], scale[2], temp_off);
    LOGI("CALIB", "temp drift: k=[%d,%d,%d] ref=%d pts=%d",
         k[0], k[1], k[2], ref, pts);
}

/* 温漂补偿量：dt(原始温度差) × k(LSB/℃×1000) / 1000，四舍五入 */
static int temp_drift(int dt, int k)
{
    long long d = (long long)dt * (long long)k;
    if (d >= 0)
        return (int)((d + 500) / 1000);
    return (int)((d - 500) / 1000);
}

void sensor_calib_apply_imu(int *ax, int *ay, int *az, int *temp_raw)
{
    int valid, off[3], scale[3], temp_off, k[3], ref;

    pthread_mutex_lock(&g_lock);
    valid = g_valid;
    memcpy(off, g_off, sizeof(off));
    memcpy(scale, g_scale, sizeof(scale));
    temp_off = g_temp_off;
    memcpy(k, g_temp_k, sizeof(k));
    ref = g_temp_ref;
    pthread_mutex_unlock(&g_lock);

    if (!valid)
        return;

    /* C3 温漂：未拟合（k=0）时补偿量为 0，行为与旧版一致 */
    int dt = temp_raw ? (*temp_raw - ref) : 0;
    int d[3] = { 0, 0, 0 };
    for (int i = 0; i < 3; i++)
        d[i] = (k[i] != 0 && dt != 0) ? temp_drift(dt, k[i]) : 0;

    if (ax)
        *ax = (*ax - d[0] - off[0]) * scale[0] / 1000;
    if (ay)
        *ay = (*ay - d[1] - off[1]) * scale[1] / 1000;
    if (az)
        *az = (*az - d[2] - off[2]) * scale[2] / 1000;
    if (temp_raw)
        *temp_raw -= temp_off;
}

int sensor_calib_start_zero_bias(int duration_ms)
{
    if (duration_ms < 500)
        duration_ms = 500;

    pthread_mutex_lock(&g_lock);
    g_collecting = 1;
    g_start_ms = now_ms();
    g_duration_ms = (uint32_t)duration_ms;
    g_n = 0;
    memset(g_sum, 0, sizeof(g_sum));
    pthread_mutex_unlock(&g_lock);

    LOGI("CALIB", "zero bias start: keep device still for %d ms", duration_ms);
    return 0;
}

int sensor_calib_is_collecting(void)
{
    pthread_mutex_lock(&g_lock);
    int v = g_collecting;
    pthread_mutex_unlock(&g_lock);
    return v;
}

int sensor_calib_poll(int ax, int ay, int az, int temp_raw)
{
    int off[3];
    int temp_avg;
    int samples;
    uint64_t now;

    pthread_mutex_lock(&g_lock);
    if (!g_collecting) {
        pthread_mutex_unlock(&g_lock);
        return 0;
    }

    g_sum[0] += ax;
    g_sum[1] += ay;
    g_sum[2] += az;
    g_sum[3] += temp_raw;
    g_n++;

    now = now_ms();
    if (now - g_start_ms < g_duration_ms || g_n < 5) {
        pthread_mutex_unlock(&g_lock);
        return 0;
    }

    off[0] = (int)(g_sum[0] / g_n);
    off[1] = (int)(g_sum[1] / g_n);
    off[2] = (int)(g_sum[2] / g_n);
    temp_avg = (int)(g_sum[3] / g_n);
    samples = g_n;
    g_collecting = 0;
    pthread_mutex_unlock(&g_lock);

    /* ---- C3 温漂：记录温度点并尝试自动拟合 ----
     * 同一温度（±1℃）覆盖旧点，避免重复点把有效温差挤出窗口。 */
    int pts_temp[TEMP_POINTS_MAX];
    int pts_off[3][TEMP_POINTS_MAX];
    int pts_flat[TEMP_POINTS_MAX * 3];
    int k_fit[3];
    int new_pts;

    pthread_mutex_lock(&g_lock);
    int slot = -1;
    for (int i = 0; i < g_pt_n; i++) {
        if (abs(g_pt_temp[i] - temp_avg) <= TEMP_SAME_RAW) { slot = i; break; }
    }
    if (slot < 0) {
        if (g_pt_n < TEMP_POINTS_MAX) {
            slot = g_pt_n++;
        } else {
            for (int i = 1; i < TEMP_POINTS_MAX; i++) {
                g_pt_temp[i - 1] = g_pt_temp[i];
                for (int a = 0; a < 3; a++)
                    g_pt_off[a][i - 1] = g_pt_off[a][i];
            }
            slot = TEMP_POINTS_MAX - 1;
        }
    }
    g_pt_temp[slot] = temp_avg;
    for (int a = 0; a < 3; a++)
        g_pt_off[a][slot] = off[a];

    new_pts = g_pt_n;
    memcpy(pts_temp, g_pt_temp, sizeof(pts_temp));
    memcpy(pts_off, g_pt_off, sizeof(pts_off));
    memcpy(k_fit, g_temp_k, sizeof(k_fit));
    int ref_old = g_temp_ref;
    pthread_mutex_unlock(&g_lock);

    for (int i = 0; i < new_pts; i++)
        for (int a = 0; a < 3; a++)
            pts_flat[i * 3 + a] = pts_off[a][i];

    int fit_ret = sensor_calib_fit(pts_temp, pts_flat, new_pts, k_fit);

    /* 已有斜率但本次温差不足：把新基线折算回参考温度，
     * 否则 apply_imu 会在新温度上重复补偿一次。 */
    if (fit_ret != 0) {
        for (int a = 0; a < 3; a++) {
            if (k_fit[a] != 0)
                off[a] -= temp_drift(temp_avg - ref_old, k_fit[a]);
        }
    }

    /* 写入持久化配置（保留用户设置的 scale 与 temp_off）。
     * 注意：静态均值包含重力分量，是“安装姿态基线”而非传感器零偏；
     * 温度静态均值是当前温度，不是偏置，因此不写入 temp_off。 */
    struct app_config c = *config_get();
    c.calib_valid = 1;
    c.imu_off[0] = off[0];
    c.imu_off[1] = off[1];
    c.imu_off[2] = off[2];
    c.temp_off = 0;
    if (fit_ret == 0) {
        c.temp_k[0] = k_fit[0];
        c.temp_k[1] = k_fit[1];
        c.temp_k[2] = k_fit[2];
        c.temp_ref = temp_avg;
    }
    c.calib_pt_n = new_pts;
    for (int i = 0; i < TEMP_POINTS_MAX; i++)
        c.calib_pt_temp[i] = pts_temp[i];
    for (int a = 0; a < 3; a++)
        for (int i = 0; i < TEMP_POINTS_MAX; i++)
            c.calib_pt_off[a][i] = pts_off[a][i];
    config_update(&c);
    config_flush();

    pthread_mutex_lock(&g_lock);
    g_valid = 1;
    memcpy(g_off, off, sizeof(g_off));
    g_temp_off = 0;
    if (fit_ret == 0) {
        memcpy(g_temp_k, k_fit, sizeof(g_temp_k));
        g_temp_ref = temp_avg;
    }
    g_pt_n = new_pts;
    memcpy(g_pt_temp, pts_temp, sizeof(g_pt_temp));
    memcpy(g_pt_off, pts_off, sizeof(g_pt_off));
    pthread_mutex_unlock(&g_lock);

    LOGI("CALIB", "static baseline done: off=[%d,%d,%d] temp_avg=%d samples=%d pts=%d",
         off[0], off[1], off[2], temp_avg, samples, new_pts);
    if (fit_ret == 0)
        LOGI("CALIB", "temp drift fitted: k=[%d,%d,%d] ref=%d",
             k_fit[0], k_fit[1], k_fit[2], temp_avg);
    else
        LOGI("CALIB", "temp drift not updated (ret=%d, need >=2 points / 2C spread)",
             fit_ret);
    return 1;
}

void sensor_calib_set(int off_x, int off_y, int off_z,
                      int scale_x1000, int scale_y1000, int scale_z1000,
                      int temp_off)
{
    struct app_config c = *config_get();
    c.calib_valid = 1;
    c.imu_off[0] = off_x;
    c.imu_off[1] = off_y;
    c.imu_off[2] = off_z;
    c.imu_scale[0] = scale_x1000;
    c.imu_scale[1] = scale_y1000;
    c.imu_scale[2] = scale_z1000;
    c.temp_off = temp_off;
    config_update(&c);
    config_flush();

    pthread_mutex_lock(&g_lock);
    g_valid = 1;
    g_off[0] = off_x;
    g_off[1] = off_y;
    g_off[2] = off_z;
    g_scale[0] = scale_x1000;
    g_scale[1] = scale_y1000;
    g_scale[2] = scale_z1000;
    g_temp_off = temp_off;
    pthread_mutex_unlock(&g_lock);

    LOGI("CALIB", "manual set: off=[%d,%d,%d] scale=[%d,%d,%d] temp_off=%d",
         off_x, off_y, off_z, scale_x1000, scale_y1000, scale_z1000, temp_off);
}

void sensor_calib_get(int *off_x, int *off_y, int *off_z,
                      int *scale_x1000, int *scale_y1000, int *scale_z1000,
                      int *temp_off)
{
    pthread_mutex_lock(&g_lock);
    if (off_x) *off_x = g_off[0];
    if (off_y) *off_y = g_off[1];
    if (off_z) *off_z = g_off[2];
    if (scale_x1000) *scale_x1000 = g_scale[0];
    if (scale_y1000) *scale_y1000 = g_scale[1];
    if (scale_z1000) *scale_z1000 = g_scale[2];
    if (temp_off) *temp_off = g_temp_off;
    pthread_mutex_unlock(&g_lock);
}

int sensor_calib_valid(void)
{
    pthread_mutex_lock(&g_lock);
    int v = g_valid;
    pthread_mutex_unlock(&g_lock);
    return v;
}

int sensor_calib_fit(const int *temps, const int *offs, int n, int k_out[3])
{
    if (!temps || !offs || !k_out || n < 2)
        return -1;

    int tmin = temps[0], tmax = temps[0];
    for (int i = 1; i < n; i++) {
        if (temps[i] < tmin) tmin = temps[i];
        if (temps[i] > tmax) tmax = temps[i];
    }
    if (tmax - tmin < TEMP_FIT_MIN_RAW)
        return -2;   /* 温差不足，拟合结果不可信 */

    double mean_t = 0.0;
    for (int i = 0; i < n; i++)
        mean_t += temps[i];
    mean_t /= (double)n;

    double var = 0.0;
    for (int i = 0; i < n; i++) {
        double d = (double)temps[i] - mean_t;
        var += d * d;
    }
    if (var <= 0.0)
        return -2;

    for (int a = 0; a < 3; a++) {
        double mean_b = 0.0;
        for (int i = 0; i < n; i++)
            mean_b += offs[i * 3 + a];
        mean_b /= (double)n;

        double cov = 0.0;
        for (int i = 0; i < n; i++)
            cov += ((double)temps[i] - mean_t) * ((double)offs[i * 3 + a] - mean_b);

        /* 斜率：LSB / 原始温度单位；换算成 LSB/℃ ×1000 */
        double slope = cov / var;
        long k = lround(slope * TEMP_LSB_PER_C * 1000.0);
        if (k > TEMP_K_LIMIT)  k = TEMP_K_LIMIT;
        if (k < -TEMP_K_LIMIT) k = -TEMP_K_LIMIT;
        k_out[a] = (int)k;
    }
    return 0;
}

void sensor_calib_get_drift(int k_out[3], int *ref_temp)
{
    pthread_mutex_lock(&g_lock);
    if (k_out)
        memcpy(k_out, g_temp_k, sizeof(g_temp_k));
    if (ref_temp)
        *ref_temp = g_temp_ref;
    pthread_mutex_unlock(&g_lock);
}

void sensor_calib_set_temp_off(int raw)
{
    if (raw < -10000) raw = -10000;
    if (raw > 10000) raw = 10000;

    struct app_config c = *config_get();
    c.temp_off = raw;
    config_update(&c);
    config_flush();

    pthread_mutex_lock(&g_lock);
    g_temp_off = raw;
    pthread_mutex_unlock(&g_lock);

    LOGI("CALIB", "temp offset set: %d raw (%.2fC)", raw, raw / 326.8);
}
