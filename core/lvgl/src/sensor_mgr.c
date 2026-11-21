/**
 * sensor_mgr.c — 传感器数据质量管理实现（T3-A）
 *
 * 处理链（每个通道独立）：
 *   范围检查 → 变化率检查 → 3σ 离群 → EMA + 中值滤波 → 状态汇总
 *
 * 异常策略：
 *   - 单次异常只记录计数，不改变全局状态（避免一个毛刺就报故障）；
 *   - 连续 3 次异常升级为故障并上报一次；
 *   - 连续异常后重置统计基线，避免环境突变（如开灯）后永久拒绝新数据；
 *   - 有效样本恢复 OK 并上报一次恢复事件。
 */
#include "sensor_mgr.h"

#include <math.h>
#include <pthread.h>
#include <string.h>
#include <time.h>

#define MED_WIN        5      /* 中值滤波窗口 */
#define STAT_WIN       16     /* 3σ 统计窗口 */
#define STAT_MIN       8      /* 统计窗口至少 8 个样本才启用 3σ */
#define INVALID_LIMIT  3      /* 连续异常/失败次数阈值 */

typedef struct {
    float alpha;              /* EMA 系数 */
    float ema;
    int   ema_init;

    int   med[MED_WIN];
    int   med_n;
    int   med_idx;

    int   stat[STAT_WIN];
    int   stat_n;
    int   stat_idx;

    int   last_raw;
    int   last_valid;
    int   invalid_cnt;
    sensor_status_t status;
} chan_t;

typedef struct {
    chan_t ch[4];             /* AP 用 3 个，IMU 用 4 个 */
    int    out[4];            /* 滤波后输出 */
    int    nch;
    uint64_t last_ms;
    uint32_t stale_ms;
    sensor_status_t current;
    int fault_pending;
    sensor_status_t pending_status;
    const char *name;
} sensor_t;

static sensor_t g_sensors[SENSOR_ID_MAX];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000L);
}

static int round_i(float v)
{
    return (int)(v >= 0.0f ? v + 0.5f : v - 0.5f);
}

static int med_value(const chan_t *c)
{
    if (c->med_n <= 0)
        return 0;

    int a[MED_WIN];
    int n = c->med_n;
    memcpy(a, c->med, sizeof(int) * (size_t)n);

    for (int i = 1; i < n; i++) {
        int k = a[i];
        int j = i - 1;
        while (j >= 0 && a[j] > k) {
            a[j + 1] = a[j];
            j--;
        }
        a[j + 1] = k;
    }
    return a[n / 2];
}

static int filter_value(const chan_t *c)
{
    if (!c->ema_init)
        return 0;

    if (c->med_n >= 3) {
        float v = (c->ema + (float)med_value(c)) * 0.5f;
        return round_i(v);
    }
    return round_i(c->ema);
}

/* 单通道处理；返回本次样本的状态（OK 表示已进入滤波） */
static sensor_status_t chan_process(chan_t *c, int raw, int lo, int hi, int spike_th)
{
    sensor_status_t result = SENSOR_OK;

    if (raw < lo || raw > hi) {
        c->invalid_cnt++;
        result = SENSOR_OUT_OF_RANGE;
    } else {
        int d = raw - c->last_raw;
        if (d < 0) d = -d;

        if (c->last_valid && d > spike_th) {
            /* 突变：更新参考值，避免后续正常样本持续被当作 spike */
            c->last_raw = raw;
            c->invalid_cnt++;
            result = SENSOR_SPIKE;
        } else if (c->stat_n >= STAT_MIN && c->invalid_cnt == 0) {
            float mean = 0.0f, var = 0.0f;
            for (int i = 0; i < c->stat_n; i++)
                mean += (float)c->stat[i];
            mean /= (float)c->stat_n;
            for (int i = 0; i < c->stat_n; i++) {
                float d2 = (float)c->stat[i] - mean;
                var += d2 * d2;
            }
            var /= (float)c->stat_n;
            float sd = sqrtf(var);

            if (sd > 0.001f && fabsf((float)raw - mean) > 3.0f * sd) {
                c->invalid_cnt++;
                result = SENSOR_OUTLIER;
            }
        }
    }

    if (result != SENSOR_OK) {
        if (c->invalid_cnt < INVALID_LIMIT)
            return result;      /* 单次异常：只报告，不改全局状态 */

        /* 连续异常：升级为故障，并重置基线以适应工况变化 */
        c->status = result;
        c->stat_n = 0;
        c->stat_idx = 0;
        c->med_n = 0;
        c->med_idx = 0;
        c->ema_init = 0;
        c->invalid_cnt = 0;
        /* 继续走下面的更新，把当前样本作为新基线 */
    } else {
        c->status = SENSOR_OK;
        c->invalid_cnt = 0;     /* 有效样本清零：避免累计失败后长期过度敏感 */
    }

    c->last_raw = raw;
    c->last_valid = 1;

    if (!c->ema_init) {
        c->ema = (float)raw;
        c->ema_init = 1;
    } else {
        c->ema = c->ema * (1.0f - c->alpha) + (float)raw * c->alpha;
    }

    c->med[c->med_idx] = raw;
    c->med_idx = (c->med_idx + 1) % MED_WIN;
    if (c->med_n < MED_WIN)
        c->med_n++;

    c->stat[c->stat_idx] = raw;
    c->stat_idx = (c->stat_idx + 1) % STAT_WIN;
    if (c->stat_n < STAT_WIN)
        c->stat_n++;

    return result;
}

static sensor_status_t worst_status(const sensor_t *s)
{
    sensor_status_t w = SENSOR_OK;
    for (int i = 0; i < s->nch; i++) {
        if (s->ch[i].status > w)
            w = s->ch[i].status;
    }
    return w;
}

static void set_status(sensor_t *s, sensor_status_t st)
{
    if (s->current == st)
        return;
    s->current = st;
    s->pending_status = st;
    s->fault_pending = 1;
}

static sensor_status_t update_common(sensor_t *s,
                                     const int *raw, const int *lo,
                                     const int *hi, const int *spike)
{
    sensor_status_t last = SENSOR_OK;

    for (int i = 0; i < s->nch; i++) {
        sensor_status_t r = chan_process(&s->ch[i], raw[i], lo[i], hi[i], spike[i]);
        if (r != SENSOR_OK)
            last = r;
        if (s->ch[i].status == SENSOR_OK)
            s->out[i] = filter_value(&s->ch[i]);
    }

    s->last_ms = now_ms();
    set_status(s, worst_status(s));
    return last;
}

void sensor_mgr_init(void)
{
    memset(g_sensors, 0, sizeof(g_sensors));

    sensor_t *ap = &g_sensors[SENSOR_ID_AP3216C];
    ap->nch = 3;
    ap->name = "AP3216C";
    ap->stale_ms = 2000;      /* 500ms × 4 */
    ap->ch[0].alpha = 0.20f;  /* ALS */
    ap->ch[1].alpha = 0.30f;  /* PS */
    ap->ch[2].alpha = 0.30f;  /* IR */

    sensor_t *imu = &g_sensors[SENSOR_ID_ICM20608];
    imu->nch = 4;
    imu->name = "ICM20608";
    imu->stale_ms = 1500;     /* 100ms × 15：容忍单核瞬时抢占，避免一次调度抖动误报 */
    imu->ch[0].alpha = 0.20f; /* AX */
    imu->ch[1].alpha = 0.20f; /* AY */
    imu->ch[2].alpha = 0.20f; /* AZ */
    imu->ch[3].alpha = 0.10f; /* TEMP */
}

void sensor_mgr_reset(void)
{
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < SENSOR_ID_MAX; i++) {
        sensor_t *s = &g_sensors[i];
        s->last_ms = 0;
        s->current = SENSOR_OK;
        s->fault_pending = 0;
        s->pending_status = SENSOR_OK;
        for (int j = 0; j < 4; j++) {
            float alpha = s->ch[j].alpha;
            memset(&s->ch[j], 0, sizeof(chan_t));
            s->ch[j].alpha = alpha;
            s->out[j] = 0;
        }
    }
    pthread_mutex_unlock(&g_lock);
}

sensor_status_t sensor_mgr_update_ap(int als, int ps, int ir)
{
    int raw[3]   = { als, ps, ir };
    int lo[3]    = { 0, 0, 0 };
    int hi[3]    = { 65535, 1023, 1023 };
    int spike[3] = { 30000, 800, 800 };

    pthread_mutex_lock(&g_lock);
    sensor_status_t st = update_common(&g_sensors[SENSOR_ID_AP3216C],
                                       raw, lo, hi, spike);
    pthread_mutex_unlock(&g_lock);
    return st;
}

sensor_status_t sensor_mgr_update_imu(int ax, int ay, int az, int temp_raw)
{
    int raw[4]   = { ax, ay, az, temp_raw };
    int lo[4]    = { -32768, -32768, -32768, -32768 };
    int hi[4]    = {  32767,  32767,  32767,  32767 };
    int spike[4] = { 20000, 20000, 20000, 5000 };

    pthread_mutex_lock(&g_lock);
    sensor_status_t st = update_common(&g_sensors[SENSOR_ID_ICM20608],
                                       raw, lo, hi, spike);
    pthread_mutex_unlock(&g_lock);
    return st;
}

void sensor_mgr_mark_fail(int sensor_id)
{
    if (sensor_id < 0 || sensor_id >= SENSOR_ID_MAX)
        return;

    pthread_mutex_lock(&g_lock);
    sensor_t *s = &g_sensors[sensor_id];
    for (int i = 0; i < s->nch; i++) {
        s->ch[i].invalid_cnt++;
        if (s->ch[i].invalid_cnt >= INVALID_LIMIT)
            s->ch[i].status = SENSOR_COMM_ERR;
    }
    set_status(s, worst_status(s));
    pthread_mutex_unlock(&g_lock);
}

void sensor_mgr_get_ap(int *als, int *ps, int *ir)
{
    pthread_mutex_lock(&g_lock);
    if (als) *als = g_sensors[SENSOR_ID_AP3216C].out[0];
    if (ps)  *ps  = g_sensors[SENSOR_ID_AP3216C].out[1];
    if (ir)  *ir  = g_sensors[SENSOR_ID_AP3216C].out[2];
    pthread_mutex_unlock(&g_lock);
}

void sensor_mgr_get_imu(int *ax, int *ay, int *az, int *temp_raw)
{
    pthread_mutex_lock(&g_lock);
    if (ax)       *ax       = g_sensors[SENSOR_ID_ICM20608].out[0];
    if (ay)       *ay       = g_sensors[SENSOR_ID_ICM20608].out[1];
    if (az)       *az       = g_sensors[SENSOR_ID_ICM20608].out[2];
    if (temp_raw) *temp_raw = g_sensors[SENSOR_ID_ICM20608].out[3];
    pthread_mutex_unlock(&g_lock);
}

sensor_status_t sensor_mgr_get_status(int sensor_id)
{
    if (sensor_id < 0 || sensor_id >= SENSOR_ID_MAX)
        return SENSOR_OK;
    pthread_mutex_lock(&g_lock);
    sensor_status_t st = g_sensors[sensor_id].current;
    pthread_mutex_unlock(&g_lock);
    return st;
}

int sensor_mgr_check_stale(void)
{
    uint64_t now = now_ms();
    int changed = 0;

    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < SENSOR_ID_MAX; i++) {
        sensor_t *s = &g_sensors[i];
        if (!s->last_ms || now - s->last_ms <= s->stale_ms)
            continue;

        int any = 0;
        for (int j = 0; j < s->nch; j++) {
            if (s->ch[j].status == SENSOR_OK) {
                s->ch[j].status = SENSOR_STALE;
                any = 1;
            }
        }
        if (any) {
            set_status(s, worst_status(s));
            changed = 1;
        }
    }
    pthread_mutex_unlock(&g_lock);
    return changed;
}

int sensor_mgr_take_fault(int *sensor_id, sensor_status_t *status,
                          char *name, int name_n)
{
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < SENSOR_ID_MAX; i++) {
        sensor_t *s = &g_sensors[i];
        if (!s->fault_pending)
            continue;

        if (sensor_id) *sensor_id = i;
        if (status)    *status = s->pending_status;
        if (name && name_n > 0) {
            strncpy(name, s->name, (size_t)name_n - 1);
            name[name_n - 1] = 0;
        }
        s->fault_pending = 0;
        pthread_mutex_unlock(&g_lock);
        return 1;
    }
    pthread_mutex_unlock(&g_lock);
    return 0;
}

const char *sensor_status_name(sensor_status_t st)
{
    switch (st) {
    case SENSOR_OK:           return "OK";
    case SENSOR_STALE:        return "STALE";
    case SENSOR_OUT_OF_RANGE: return "OUT_OF_RANGE";
    case SENSOR_SPIKE:        return "SPIKE";
    case SENSOR_OUTLIER:      return "OUTLIER";
    case SENSOR_COMM_ERR:     return "COMM_ERR";
    case SENSOR_SATURATED:    return "SATURATED";
    default:                  return "?";
    }
}
