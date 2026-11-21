/**
 * sensor_pipeline.c — 传感器采集流水线实现（infra 层）
 *
 * 由 dev_bridge.c 抽出：IIO/KEY 采集线程、校准、滤波、双阈值判定、诊断快照。
 * 设备访问走 infra/sensor_io，判定逻辑复用 domain/warn_rule。
 */
#include "sensor_pipeline.h"
#include "sensor_io.h"
#include "../log_svc.h"
#include "../sensor_mgr.h"
#include "../sensor_calib.h"
#include "../domain/sensors/imu_math.h"
#include "../domain/warn_rule.h"
#include "../fusion.h"
#include "../thread_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <time.h>
#include <pthread.h>
#include <poll.h>
#include <linux/input.h>

/* ---- KEY0：gpio-keys input 子系统（dts key-input 节点，code=227 KEY_ARMED）---- */
#define KEY_DEV_NAME  "imx6ull-gpio-keys"
#ifndef KEY_ARMED
#define KEY_ARMED     227
#endif

/* ---- 阈值/周期 ---- */
#define POLL_MS          100     /* KEY0 poll 超时 = 采集循环节拍 */
#define ALS_PERIOD       1       /* 每 N 个节拍读一次 ap3216c（100ms，10Hz） */
#define IMU_PERIOD       1       /* 每 N 个节拍读一次 icm20608（100ms，10Hz） */

/* ---- 线程生命周期 ---- */
static volatile int s_quit = 0;
static pthread_t s_imu_tid, s_collect_tid;
static int s_imu_started = 0, s_collect_started = 0;
static volatile int s_imu_buffer_ok = 0;   /* 1=IMU buffer 通路可用 */

/* ---- 预警阈值配置（设置弹窗可调，采集线程读取） ---- */
static struct dev_config g_cfg = {
    .zones = {
        /* 前门:   AP3216C 距离 > 30 告警；> 20 只预警（双阈值） */
        { .src = WARN_SRC_AP3216C, .channel = AP_DATA_PS, .threshold = 30,
          .threshold_warn = 20, .cmp = WARN_CMP_GT, .enabled = 1 },
        /* 后门:   默认不触发 */
        { .src = WARN_SRC_NONE, .channel = 0, .threshold = 0,
          .threshold_warn = 0, .cmp = WARN_CMP_GT, .enabled = 0 },
        /* 窗户:   默认不触发 */
        { .src = WARN_SRC_NONE, .channel = 0, .threshold = 0,
          .threshold_warn = 0, .cmp = WARN_CMP_GT, .enabled = 0 },
        /* 仓库:   ICM 去重力线性加速度 > 30 告警；> 15 只预警
         * （旧版为合加速度 > 150，阈值语义已随 C3 切换） */
        { .src = WARN_SRC_ICM, .channel = IMU_DATA_LIN, .threshold = 30,
          .threshold_warn = 15, .cmp = WARN_CMP_GT, .enabled = 1 },
    },
    .debounce = 2,
    .cooldown_sec = 60,   /* 消警后 60s 冷却，期间不重复触发 */
};
static volatile int s_ap_ps_off = 0;      /* AP3216C 距离本底 */
static volatile int s_ap_ir_off = 0;      /* AP3216C 红外本底 */
static volatile int s_ap_als_scale = 1000;/* AP3216C 光照比例 ×1000 */

/* ---- 共享状态（采集线程写 / LVGL 定时器读） ---- */
struct dev_shm {
    pthread_mutex_t lock;
    int key_toggle;        /* KEY0 按下事件计数（消费后清零） */
    int warn_hit[4];       /* 各位置告警触发标志（消费后清零） */
    int warn_pending[4];   /* C2 双阈值：各位置预警标志（只轻提醒，消费后清零） */
    /* ---- 诊断快照（采集线程持续刷新） ---- */
    int key_ok, als_ok, imu_ok;   /* 设备在位 */
    int key_level;                /* KEY0 当前电平 1=按下 */
    int als_ir, als_als, als_ps;  /* AP3216C 最新原始值 */
    int imu_ax, imu_ay, imu_az;   /* ICM20608 最新原始值 */
    int imu_temp_x10;             /* 温度 ℃×10 */
    int imu_mag_x100;             /* C2：合加速度 ×100 */
    int imu_rms_x100;             /* C2：滑窗 RMS ×100 */
    int imu_peak_x100;            /* C2：滑窗峰值 ×100 */
    int imu_lin_x100;             /* C3：线性加速度（去重力）×100 */
    /* ---- 测试面板使能（手动开关） ---- */
    int als_en, imu_en;           /* 采集使能 1=开 */
    /* ---- 校准完成标志（采集线程写，UI 定时器消费） ---- */
    int calib_done;               /* 1=零偏校准完成 */
};

static struct dev_shm g_shm = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
};
static int g_started = 0;

/* C2/C3：纯算法模块（去重力 + 滑窗 RMS/峰值） */
static imu_math_t s_imu_math;

static int imu_linear_locked(int ax, int ay, int az)
{
    return imu_math_linear(&s_imu_math, ax, ay, az);
}

static void imu_mag_stats_locked(int lin_x100)
{
    int rms = 0, peak = 0;
    imu_math_stats(&s_imu_math, lin_x100, &rms, &peak);
    g_shm.imu_rms_x100 = rms;
    g_shm.imu_peak_x100 = peak;
}

static uint32_t now_ms32(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000ULL +
                      (uint64_t)(ts.tv_nsec / 1000000L));
}

/* C1/C5：单源事件入口。融合开启时只登记事件（由 fusion_poll 统一定案），
 * 关闭时保持原语义——立即置位 warn_hit。 */
static void zone_hit(int z)
{
    if (g_cfg.fusion_on) {
        fusion_add(z, FUSE_SRC_SENSOR, now_ms32(), 55);
        LOGD("FUSE", "zone=%d sensor event queued", z);
    } else {
        pthread_mutex_lock(&g_shm.lock);
        g_shm.warn_hit[z]++;
        pthread_mutex_unlock(&g_shm.lock);
    }
}

/* C2 双阈值：比较函数由 domain/warn_rule 提供（warn_rule_match） */

/* C2 双阈值：仅登记“预警”标志，由 LVGL 定时器轻提醒（不触发告警/声光） */
static void zone_warn_pending(int z)
{
    pthread_mutex_lock(&g_shm.lock);
    g_shm.warn_pending[z]++;
    pthread_mutex_unlock(&g_shm.lock);
}

/* C2 双阈值/防抖：阈值判定状态（纯逻辑在 domain/warn_rule） */
static warn_rule_t g_rule;


static void *imu_thread(void *arg)
{
    (void)arg;
    int fd = -1;
    char devpath[64] = {0};
    char sysfs[128] = {0};

    while (!s_quit) {
        if (fd < 0) {
            fd = sio_imu_buffer_open(devpath, sizeof(devpath),
                                 sysfs, sizeof(sysfs));
            if (fd < 0) {
                sleep(1);
                continue;
            }
            __atomic_store_n(&s_imu_buffer_ok, 1, __ATOMIC_RELEASE);
            pthread_mutex_lock(&g_shm.lock);
            g_shm.imu_ok = 1;
            pthread_mutex_unlock(&g_shm.lock);
            LOGI("IMU", "buffer enabled: %s (100Hz)", devpath);
        }

        /* T5-B：先 poll 再读，保证退出标志在 200ms 内生效（read 本身会无限阻塞） */
        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        int pr = poll(&pfd, 1, 200);
        if (pr <= 0)
            continue;              /* 超时/中断：回循环顶检查 s_quit */

        int16_t buf[16];
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n < 14) {
            /* 当前驱动没有时间戳，单帧 16 字节（7×s16 + padding）；
             * 只要拿到 14 字节有效通道数据就够用。 */
            char path[192];
            snprintf(path, sizeof(path), "%s/buffer/enable", sysfs);
            sio_write_sysfs(path, "0");
            close(fd);
            fd = -1;
            __atomic_store_n(&s_imu_buffer_ok, 0, __ATOMIC_RELEASE);
            pthread_mutex_lock(&g_shm.lock);
            g_shm.imu_ok = 0;
            pthread_mutex_unlock(&g_shm.lock);
            LOGE("IMU", "buffer read %d bytes: %s, retry",
                 (int)n, strerror(errno));
            sleep(1);
            continue;
        }

        int ax = buf[0];
        int ay = buf[1];
        int az = buf[2];
        int temp = buf[3];

        if (sensor_calib_poll(ax, ay, az, temp) == 1) {
            pthread_mutex_lock(&g_shm.lock);
            g_shm.calib_done = 1;
            pthread_mutex_unlock(&g_shm.lock);
        }

        sensor_calib_apply_imu(&ax, &ay, &az, &temp);
        sensor_mgr_update_imu(ax, ay, az, temp);
        sensor_mgr_get_imu(&ax, &ay, &az, &temp);

        int temp_x10 = (int)((double)temp / 326.8 * 10.0 + 250.0);
        double m2 = (double)ax * ax + (double)ay * ay + (double)az * az;
        int mag_x100 = (int)(sqrt(m2) / 32768.0 * 16.0 * 100.0);

        pthread_mutex_lock(&g_shm.lock);
        g_shm.imu_ax = ax;
        g_shm.imu_ay = ay;
        g_shm.imu_az = az;
        g_shm.imu_temp_x10 = temp_x10;
        int lin_x100 = imu_linear_locked(ax, ay, az);
        g_shm.imu_mag_x100 = mag_x100;
        g_shm.imu_lin_x100 = lin_x100;
        imu_mag_stats_locked(lin_x100);
        pthread_mutex_unlock(&g_shm.lock);
    }

    /* 退出清理：关闭 buffer（magic 语义由驱动处理） */
    if (fd >= 0) {
        char path[192];
        snprintf(path, sizeof(path), "%s/buffer/enable", sysfs);
        sio_write_sysfs(path, "0");
        close(fd);
    }
    __atomic_store_n(&s_imu_buffer_ok, 0, __ATOMIC_RELEASE);
    LOGI("IMU", "buffer thread stopped");
    return NULL;
}

static void *collect_thread(void *arg)
{
    (void)arg;
    int key_fd = sio_find_key(KEY_DEV_NAME);

    /* AP3216C / ICM20608（IIO 框架）：探测设备目录并打开通道属性文件 */
    char als_path[96] = {0}, imu_path[96] = {0};
    int als_found = (sio_find_iio("ap3216c", als_path, sizeof(als_path)) == 0);
    int imu_found = (sio_find_iio("icm20608", imu_path, sizeof(imu_path)) == 0);

    int als_als_fd = -1, als_ps_fd = -1, als_ir_fd = -1;
    int imu_ax_fd = -1, imu_ay_fd = -1, imu_az_fd = -1, imu_t_fd = -1;
    int als_fd = -1, imu_fd = -1;   /* 综合在位标志（沿用原命名） */

    if (als_found) {
        /* 属性名以内核 IIO 命名为准（IIO_INTENSITY → in_intensity_*） */
        als_als_fd = sio_open_attr(als_path, "in_intensity_both_raw");
        als_ps_fd  = sio_open_attr(als_path, "in_proximity_raw");
        als_ir_fd  = sio_open_attr(als_path, "in_intensity_ir_raw");
        als_fd = (als_als_fd >= 0 && als_ps_fd >= 0 && als_ir_fd >= 0) ? 0 : -1;
    }
    if (imu_found) {
        imu_ax_fd = sio_open_attr(imu_path, "in_accel_x_raw");
        imu_ay_fd = sio_open_attr(imu_path, "in_accel_y_raw");
        imu_az_fd = sio_open_attr(imu_path, "in_accel_z_raw");
        imu_t_fd  = sio_open_attr(imu_path, "in_temp_raw");
        imu_fd = (imu_ax_fd >= 0 && imu_ay_fd >= 0 &&
                  imu_az_fd >= 0 && imu_t_fd >= 0) ? 0 : -1;
    }

    if (key_fd < 0)
        LOGW("BRIDGE", "%s not found (KEY0 disabled; dts key-input okay + insmod key_input.ko?)",
             KEY_DEV_NAME);
    if (als_fd < 0)
        LOGW("BRIDGE", "IIO ap3216c not found (ALS/PS disabled; insmod ap3216c.ko?)");
    if (imu_fd < 0)
        LOGW("BRIDGE", "IIO icm20608 not found (IMU disabled; insmod icm20608.ko?)");

    /* 在位标志写入诊断快照 */
    pthread_mutex_lock(&g_shm.lock);
    g_shm.key_ok = (key_fd >= 0);
    g_shm.als_ok = (als_fd >= 0);
    g_shm.imu_ok = (imu_fd >= 0);
    pthread_mutex_unlock(&g_shm.lock);

    int tick = 0;

    while (!s_quit) {
        /* --- KEY0（gpio-keys input 流）：poll 等待（100ms 超时兼作循环节拍） --- */
        if (key_fd >= 0) {
            struct pollfd pfd = { .fd = key_fd, .events = POLLIN };
            if (poll(&pfd, 1, POLL_MS) > 0) {
                struct input_event ev;
                /* 每次读一条完整 input_event；EV_KEY+KEY_ARMED+value=1 即按下 */
                while (read(key_fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
                    if (ev.type == EV_KEY && ev.code == KEY_ARMED) {
                        LOGD("BRIDGE", "KEY0 event value=%d", ev.value);
                        pthread_mutex_lock(&g_shm.lock);
                        g_shm.key_level = (ev.value == 1);
                        if (ev.value == 1)
                            g_shm.key_toggle++;
                        pthread_mutex_unlock(&g_shm.lock);
                    }
                }
            }
        } else {
            usleep(POLL_MS * 1000);
        }

        tick++;

        /* --- IIO 设备重连（T3-A）：驱动 rmmod/insmod 或设备节点重建后旧 fd 失效，
         * 每 1s 重新探测并打开属性文件；成功后 sensor_mgr 收到有效样本会自动恢复 OK。 --- */
        if (als_fd < 0 && (tick % 10) == 0) {
            /* 清理可能残留的半开句柄，避免重复 open 泄漏 */
            if (als_als_fd >= 0) { close(als_als_fd); als_als_fd = -1; }
            if (als_ps_fd >= 0)  { close(als_ps_fd);  als_ps_fd = -1; }
            if (als_ir_fd >= 0)  { close(als_ir_fd);  als_ir_fd = -1; }

            als_found = (sio_find_iio("ap3216c", als_path, sizeof(als_path)) == 0);
            if (als_found) {
                als_als_fd = sio_open_attr(als_path, "in_intensity_both_raw");
                als_ps_fd  = sio_open_attr(als_path, "in_proximity_raw");
                als_ir_fd  = sio_open_attr(als_path, "in_intensity_ir_raw");
                if (als_als_fd >= 0 && als_ps_fd >= 0 && als_ir_fd >= 0) {
                    als_fd = 0;
                    LOGI("BRIDGE", "AP3216C reconnected: %s", als_path);
                } else {
                    if (als_als_fd >= 0) close(als_als_fd);
                    if (als_ps_fd >= 0) close(als_ps_fd);
                    if (als_ir_fd >= 0) close(als_ir_fd);
                    als_als_fd = als_ps_fd = als_ir_fd = -1;
                }
            }
            pthread_mutex_lock(&g_shm.lock);
            g_shm.als_ok = (als_fd >= 0);
            pthread_mutex_unlock(&g_shm.lock);
        }

        if (imu_fd < 0 && (tick % 10) == 0) {
            if (imu_ax_fd >= 0) { close(imu_ax_fd); imu_ax_fd = -1; }
            if (imu_ay_fd >= 0) { close(imu_ay_fd); imu_ay_fd = -1; }
            if (imu_az_fd >= 0) { close(imu_az_fd); imu_az_fd = -1; }
            if (imu_t_fd >= 0)  { close(imu_t_fd);  imu_t_fd = -1; }

            imu_found = (sio_find_iio("icm20608", imu_path, sizeof(imu_path)) == 0);
            if (imu_found) {
                imu_ax_fd = sio_open_attr(imu_path, "in_accel_x_raw");
                imu_ay_fd = sio_open_attr(imu_path, "in_accel_y_raw");
                imu_az_fd = sio_open_attr(imu_path, "in_accel_z_raw");
                imu_t_fd  = sio_open_attr(imu_path, "in_temp_raw");
                if (imu_ax_fd >= 0 && imu_ay_fd >= 0 && imu_az_fd >= 0 && imu_t_fd >= 0) {
                    imu_fd = 0;
                    LOGI("BRIDGE", "ICM20608 reconnected: %s", imu_path);
                } else {
                    if (imu_ax_fd >= 0) close(imu_ax_fd);
                    if (imu_ay_fd >= 0) close(imu_ay_fd);
                    if (imu_az_fd >= 0) close(imu_az_fd);
                    if (imu_t_fd >= 0) close(imu_t_fd);
                    imu_ax_fd = imu_ay_fd = imu_az_fd = imu_t_fd = -1;
                }
            }
            pthread_mutex_lock(&g_shm.lock);
            g_shm.imu_ok = (imu_fd >= 0);
            pthread_mutex_unlock(&g_shm.lock);
        }

        /* ================================================================
         * 预警判定：每个安防位置按各自配置（触发源+数据通道+阈值）独立判定。
         * AP3216C 数据 500ms 刷新一次，ICM 数据 100ms 刷新一次（T3-A 10Hz）；
         * 数据刷新时对所有引用该传感器的位置做去抖判定。
         * ================================================================ */

        /* --- AP3216C 数据刷新（IIO sysfs：ALS/PS/IR 三通道） --- */
        if (als_fd >= 0 && (tick % ALS_PERIOD) == 0) {
            int als = 0, ps = 0, ir = 0;
            if (sio_read_int(als_als_fd, &als) == 0 &&
                sio_read_int(als_ps_fd, &ps) == 0 &&
                sio_read_int(als_ir_fd, &ir) == 0) {

                /* AP3216C 校准：PS/IR 去环境本底，ALS 比例 */
                ps -= s_ap_ps_off;
                ir -= s_ap_ir_off;
                if (ps < 0) ps = 0;
                if (ir < 0) ir = 0;
                als = als * s_ap_als_scale / 1000;
                if (als < 0) als = 0;

                /* T3-A：范围/变化率/3σ 剔除 + EMA/中值滤波；
                 * 后续阈值判定与诊断快照使用滤波后的值。 */
                sensor_mgr_update_ap(als, ps, ir);
                sensor_mgr_get_ap(&als, &ps, &ir);

                pthread_mutex_lock(&g_shm.lock);
                g_shm.als_ir = ir;
                g_shm.als_als = als;
                g_shm.als_ps = ps;
                int als_en = g_shm.als_en;
                pthread_mutex_unlock(&g_shm.lock);

                for (int z = 0; z < 4; z++) {
                    if (!als_en) break;
                    const struct zone_warn_cfg *c = &g_cfg.zones[z];
                    if (!c->enabled || c->src != WARN_SRC_AP3216C) continue;
                    int val = (c->channel == AP_DATA_PS) ? ps :
                              (c->channel == AP_DATA_ALS) ? als : ir;

                    warn_rule_evt_t ev = warn_rule_eval(&g_rule, z, c, val, g_cfg.debounce);
                    if (ev == WARN_RULE_PENDING)
                        zone_warn_pending(z);
                    else if (ev == WARN_RULE_HIT)
                        zone_hit(z);   /* 融合开启时只登记事件 */
                }
            } else {
                sensor_mgr_mark_fail(SENSOR_ID_AP3216C);
                /* 连续失败达到 COMM_ERR 后再关闭 fd 并交给重连块；
                 * 单次瞬时失败保持 fd，避免误判。 */
                if (sensor_mgr_get_status(SENSOR_ID_AP3216C) == SENSOR_COMM_ERR) {
                    if (als_als_fd >= 0) close(als_als_fd);
                    if (als_ps_fd >= 0) close(als_ps_fd);
                    if (als_ir_fd >= 0) close(als_ir_fd);
                    als_als_fd = als_ps_fd = als_ir_fd = -1;
                    als_fd = -1;
                }
            }
        }

        /* --- ICM20608 数据刷新（buffer 不可用时回退 sysfs：accel xyz + temp） --- */
        if (!__atomic_load_n(&s_imu_buffer_ok, __ATOMIC_ACQUIRE) && imu_fd >= 0 &&
            (tick % IMU_PERIOD) == 0) {
            int ax = 0, ay = 0, az = 0, temp_raw = 0;
            if (sio_read_int(imu_ax_fd, &ax) == 0 &&
                sio_read_int(imu_ay_fd, &ay) == 0 &&
                sio_read_int(imu_az_fd, &az) == 0 &&
                sio_read_int(imu_t_fd, &temp_raw) == 0) {

                /* T3-C：零偏采集使用原始值；随后应用校准，再进入 T3-A 滤波。
                 * 采集完成标志交给 UI 定时器在主线程上报（采集线程不碰 UI）。 */
                if (sensor_calib_poll(ax, ay, az, temp_raw) == 1) {
                    pthread_mutex_lock(&g_shm.lock);
                    g_shm.calib_done = 1;
                    pthread_mutex_unlock(&g_shm.lock);
                }
                sensor_calib_apply_imu(&ax, &ay, &az, &temp_raw);

                sensor_mgr_update_imu(ax, ay, az, temp_raw);
                sensor_mgr_get_imu(&ax, &ay, &az, &temp_raw);

                int temp_x10 = (int)((double)temp_raw / 326.8 * 10.0 + 250.0);
                double m2 = (double)ax * ax + (double)ay * ay + (double)az * az;
                int mag_x100 = (int)(sqrt(m2) / 32768.0 * 16.0 * 100.0);

                pthread_mutex_lock(&g_shm.lock);
                g_shm.imu_ok = 1;
                g_shm.imu_ax = ax;
                g_shm.imu_ay = ay;
                g_shm.imu_az = az;
                g_shm.imu_temp_x10 = temp_x10;
                int lin_x100 = imu_linear_locked(ax, ay, az);
                g_shm.imu_mag_x100 = mag_x100;
                g_shm.imu_lin_x100 = lin_x100;
                imu_mag_stats_locked(lin_x100);
                pthread_mutex_unlock(&g_shm.lock);
            } else {
                pthread_mutex_lock(&g_shm.lock);
                g_shm.imu_ok = 0;
                pthread_mutex_unlock(&g_shm.lock);

                sensor_mgr_mark_fail(SENSOR_ID_ICM20608);
                if (sensor_mgr_get_status(SENSOR_ID_ICM20608) == SENSOR_COMM_ERR) {
                    if (imu_ax_fd >= 0) close(imu_ax_fd);
                    if (imu_ay_fd >= 0) close(imu_ay_fd);
                    if (imu_az_fd >= 0) close(imu_az_fd);
                    if (imu_t_fd >= 0) close(imu_t_fd);
                    imu_ax_fd = imu_ay_fd = imu_az_fd = imu_t_fd = -1;
                    imu_fd = -1;
                }
            }
        }

        /* --- C2/C3：IMU 预警判定（统一读 g_shm，buffer 与 sysfs 两种通路都生效）
         * IMU_DATA_LIN/MAG 使用去重力线性加速度（静止≈0），阈值不受安装角度影响。 --- */
        if ((tick % IMU_PERIOD) == 0) {
            int ax, ay, az, temp_x10, imu_en, imu_ok, lin_x100;

            pthread_mutex_lock(&g_shm.lock);
            ax = g_shm.imu_ax;
            ay = g_shm.imu_ay;
            az = g_shm.imu_az;
            temp_x10 = g_shm.imu_temp_x10;
            lin_x100 = g_shm.imu_lin_x100;
            imu_en = g_shm.imu_en;
            imu_ok = g_shm.imu_ok;
            pthread_mutex_unlock(&g_shm.lock);

            if (imu_ok && imu_en) {
                long long ax_x100 = llabs((long long)ax * 100 / 2048);
                long long ay_x100 = llabs((long long)ay * 100 / 2048);
                long long az_x100 = llabs((long long)az * 100 / 2048);

                for (int z = 0; z < 4; z++) {
                    const struct zone_warn_cfg *c = &g_cfg.zones[z];
                    if (!c->enabled || c->src != WARN_SRC_ICM) continue;
                    long long val;
                    switch (c->channel) {
                    case IMU_DATA_AX:   val = ax_x100; break;
                    case IMU_DATA_AY:   val = ay_x100; break;
                    case IMU_DATA_AZ:   val = az_x100; break;
                    case IMU_DATA_TEMP: val = temp_x10; break;
                    default:            val = lin_x100; break;   /* MAG/LIN：去重力 */
                    }

                    warn_rule_evt_t ev = warn_rule_eval(&g_rule, z, c, val, g_cfg.debounce);
                    if (ev == WARN_RULE_PENDING)
                        zone_warn_pending(z);
                    else if (ev == WARN_RULE_HIT)
                        zone_hit(z);   /* 融合开启时只登记事件 */
                }
            }
        }

        /* C2：每 5s 打一条统计（DEBUG 级），便于观察 RMS/峰值 */
        if ((tick % 50) == 0) {
            int mag, rms, peak, lin, ok;
            pthread_mutex_lock(&g_shm.lock);
            mag = g_shm.imu_mag_x100;
            rms = g_shm.imu_rms_x100;
            peak = g_shm.imu_peak_x100;
            lin = g_shm.imu_lin_x100;
            ok = g_shm.imu_ok;
            pthread_mutex_unlock(&g_shm.lock);
            if (ok)
                LOGD("BRIDGE", "imu mag=%d lin=%d rms=%d peak=%d", mag, lin, rms, peak);
        }
    }
    return NULL;
}


/* ================================================================
 * 对外接口
 * ================================================================ */
void sensor_pipeline_set_config(const struct dev_config *cfg)
{
    if (!cfg) return;
    pthread_mutex_lock(&g_shm.lock);
    g_cfg = *cfg;
    pthread_mutex_unlock(&g_shm.lock);
}

const struct dev_config *sensor_pipeline_get_config(void)
{
    return &g_cfg;
}

void sensor_pipeline_set_ap_calib(int ps_off, int ir_off, int als_scale_x1000)
{
    s_ap_ps_off = ps_off;
    s_ap_ir_off = ir_off;
    s_ap_als_scale = (als_scale_x1000 > 0) ? als_scale_x1000 : 1000;
}

void sensor_pipeline_get_ap_calib(int *ps_off, int *ir_off, int *als_scale_x1000)
{
    if (ps_off) *ps_off = s_ap_ps_off;
    if (ir_off) *ir_off = s_ap_ir_off;
    if (als_scale_x1000) *als_scale_x1000 = s_ap_als_scale;
}

void sensor_pipeline_set_als_en(int en)
{
    pthread_mutex_lock(&g_shm.lock);
    g_shm.als_en = en ? 1 : 0;
    pthread_mutex_unlock(&g_shm.lock);
}

void sensor_pipeline_set_imu_en(int en)
{
    pthread_mutex_lock(&g_shm.lock);
    g_shm.imu_en = en ? 1 : 0;
    pthread_mutex_unlock(&g_shm.lock);
}

int sensor_pipeline_get_als_en(void)
{
    int v;
    pthread_mutex_lock(&g_shm.lock);
    v = g_shm.als_en;
    pthread_mutex_unlock(&g_shm.lock);
    return v;
}

int sensor_pipeline_get_imu_en(void)
{
    int v;
    pthread_mutex_lock(&g_shm.lock);
    v = g_shm.imu_en;
    pthread_mutex_unlock(&g_shm.lock);
    return v;
}

int sensor_pipeline_take_key_toggle(void)
{
    int v;
    pthread_mutex_lock(&g_shm.lock);
    v = g_shm.key_toggle;
    g_shm.key_toggle = 0;
    pthread_mutex_unlock(&g_shm.lock);
    return v;
}

int sensor_pipeline_take_warn(int zone)
{
    if (zone < 0 || zone >= 4) return 0;
    int v;
    pthread_mutex_lock(&g_shm.lock);
    v = g_shm.warn_hit[zone];
    g_shm.warn_hit[zone] = 0;
    pthread_mutex_unlock(&g_shm.lock);
    return v;
}

int sensor_pipeline_take_warn_pending(int zone)
{
    if (zone < 0 || zone >= 4) return 0;
    int v;
    pthread_mutex_lock(&g_shm.lock);
    v = g_shm.warn_pending[zone];
    g_shm.warn_pending[zone] = 0;
    pthread_mutex_unlock(&g_shm.lock);
    return v;
}

int sensor_pipeline_take_calib_done(void)
{
    int v;
    pthread_mutex_lock(&g_shm.lock);
    v = g_shm.calib_done;
    g_shm.calib_done = 0;
    pthread_mutex_unlock(&g_shm.lock);
    return v;
}

void sensor_pipeline_get_diag(struct dev_diag *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));

    pthread_mutex_lock(&g_shm.lock);
    out->key_ok       = g_shm.key_ok;
    out->als_ok       = g_shm.als_ok;
    out->imu_ok       = g_shm.imu_ok;
    out->key_pressed  = g_shm.key_level ? 1 : 0;
    out->als_ir       = g_shm.als_ir;
    out->als_als      = g_shm.als_als;
    out->als_ps       = g_shm.als_ps;
    out->imu_ax       = g_shm.imu_ax;
    out->imu_ay       = g_shm.imu_ay;
    out->imu_az       = g_shm.imu_az;
    out->imu_temp_x10 = g_shm.imu_temp_x10;
    out->imu_mag_x100 = g_shm.imu_mag_x100;
    out->imu_rms_x100 = g_shm.imu_rms_x100;
    out->imu_peak_x100 = g_shm.imu_peak_x100;
    out->imu_lin_x100 = g_shm.imu_lin_x100;
    out->als_en       = g_shm.als_en;
    out->imu_en       = g_shm.imu_en;
    pthread_mutex_unlock(&g_shm.lock);
}

int sensor_pipeline_imu_buffer_ok(void)
{
    return __atomic_load_n(&s_imu_buffer_ok, __ATOMIC_ACQUIRE);
}

int sensor_pipeline_start(void)
{
    if (s_imu_started || s_collect_started)
        return 0;

    s_quit = 0;

    /* 采集默认开启 */
    pthread_mutex_lock(&g_shm.lock);
    g_shm.als_en = 1;
    g_shm.imu_en = 1;
    pthread_mutex_unlock(&g_shm.lock);

    imu_math_reset(&s_imu_math);
    if (thread_start(&s_imu_tid, 256 * 1024, imu_thread, NULL) == 0)
        s_imu_started = 1;
    else
        LOGW("IMU", "buffer thread create failed, fallback sysfs");

    if (thread_start(&s_collect_tid, 512 * 1024, collect_thread, NULL) != 0) {
        LOGE("PIPELINE", "collect thread create failed");
        return -1;
    }
    s_collect_started = 1;
    return 0;
}

void sensor_pipeline_stop(void)
{
    if (!s_collect_started && !s_imu_started)
        return;

    s_quit = 1;

    if (s_collect_started) {
        pthread_join(s_collect_tid, NULL);
        s_collect_started = 0;
    }
    if (s_imu_started) {
        pthread_join(s_imu_tid, NULL);
        s_imu_started = 0;
    }
}
