/**
 * dev_bridge.c — 外设信号桥接层实现
 *
 * 信号映射（板载驱动 → UI）：
 *   /dev/input/eventX                 KEY0(gpio-keys, KEY_ARMED) 按下 → 屏幕唤醒
 *   IIO ap3216c（sysfs）              距离靠近（布防中，防抖）→ 前门告警
 *   IIO icm20608（sysfs）             震动（布防中，防抖）→ 仓库告警
 *   /dev/led                          LED 跟随布防状态（告警闪烁/布防常亮）
 *   /sys/class/leds/beep/brightness   板载蜂鸣器跟随告警状态（出厂 dts gpio-leds）
 */
#include "dev_bridge.h"
#include "cam_feed.h"
#include "detector.h"
#include "cloud_detect.h"
#include "config_mgr.h"
#include "log_svc.h"
#include "sensor_mgr.h"
#include "sensor_calib.h"
#include "health_mgr.h"
#include "event_bus.h"
#include "alarm_fsm.h"
#include "domain/zone_state.h"
#include "domain/sensors/imu_math.h"
#include "domain/alarm/warn_policy.h"
#include "domain/warn_rule.h"
#include "infra/hal_io.h"
#include "infra/sensor_pipeline.h"
#include "thread_util.h"
#include "fusion.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <dirent.h>
#include <time.h>
#include <pthread.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <linux/input.h>

/* ---- KEY0：gpio-keys input 子系统（dts key-input 节点，code=227 KEY_ARMED）---- */
#define KEY_DEV_NAME  "imx6ull-gpio-keys"
#ifndef KEY_ARMED
#define KEY_ARMED     227   /* 4.1.15 工具链头文件可能缺失，兜底（与驱动一致） */
#endif

/* ---- 设备节点 ---- */
/* AP3216C/ICM20608 已迁移 IIO 框架（P7-4）：用户态走
 * /sys/bus/iio/devices/iio:deviceX/ 下的通道属性文件（见 find_iio_path） */
#define IIO_SYSFS   "/sys/bus/iio/devices"

/* 探测 IIO 设备：遍历 iio:device*，读 name 属性匹配（如 "ap3216c"）。
 * 找到则输出设备目录路径（如 /sys/bus/iio/devices/iio:device0） */
static int find_iio_path(const char *dev_name, char *out, size_t len)
{
    DIR *dir = opendir(IIO_SYSFS);
    struct dirent *ent;
    char npath[128];
    char name[64];
    int ret = -1;

    if (!dir) return -1;
    while ((ent = readdir(dir)) != NULL) {
        if (strncmp(ent->d_name, "iio:device", 10) != 0) continue;
        snprintf(npath, sizeof(npath), IIO_SYSFS "/%s/name", ent->d_name);
        int fd = open(npath, O_RDONLY);
        if (fd < 0) continue;
        memset(name, 0, sizeof(name));
        if (read(fd, name, sizeof(name) - 1) > 0) {
            char *nl = strchr(name, '\n');
            if (nl) *nl = '\0';
            if (strcmp(name, dev_name) == 0) {
                snprintf(out, len, IIO_SYSFS "/%s", ent->d_name);
                ret = 0;
            }
        }
        close(fd);
        if (ret == 0) break;
    }
    closedir(dir);
    return ret;
}

/* 打开 IIO 通道属性文件（保持 fd，读取时 lseek 回开头） */
static int open_iio_attr(const char *dev_path, const char *attr)
{
    char path[160];
    snprintf(path, sizeof(path), "%s/%s", dev_path, attr);
    return open(path, O_RDONLY);
}

/* 读 IIO 属性整数值（sysfs ASCII → int） */
static int read_iio_int(int fd, int *val)
{
    char buf[32] = {0};
    int n;

    if (fd < 0) return -1;
    if (lseek(fd, 0, SEEK_SET) < 0) return -1;
    n = read(fd, buf, sizeof(buf) - 1);
    if (n <= 0) return -1;
    *val = atoi(buf);
    return 0;
}

/* ---- 阈值/周期 ---- */
#define POLL_MS          100     /* KEY0 poll 超时 = 采集循环节拍 */
#define ALS_PERIOD       1       /* 每 N 个节拍读一次 ap3216c（100ms，10Hz） */
#define IMU_PERIOD       1       /* 每 N 个节拍读一次 icm20608（100ms，10Hz；T3-A） */

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
static int g_cfg_loaded = 0;   /* 是否已从持久化配置恢复（UI 可能早于 dev_bridge_start 读取） */
static int g_started = 0;      /* dev_bridge_start 幂等标志 */

/* 单调时钟（融合/轮询使用） */
static uint32_t now_ms32(void);

/* ---- 告警声光策略（分级节拍/手动锁定/静音，纯逻辑见 domain/alarm） ---- */
static warn_policy_t g_warn;
static time_t g_zone_last_trigger[4] = {0};    /* 各位置上次触发时间（冷却用） */
static time_t g_zone_last_warn[4] = {0};       /* C2 双阈值：各位置上次预警时间（冷却用） */

static int g_pre_result = DET_RESULT_NONE;  /* 两级管线：当前事件初步结论（FINAL 定案参考） */

/* 云端入侵类型键 → 时间轴用词（server.py 归一：person/animal/object） */
static const char *det_type_word(const char *key)
{
    if (!key || !key[0]) return "";
    if (!strcmp(key, "person")) return "人员";
    if (!strcmp(key, "animal")) return "动物";
    if (!strcmp(key, "object")) return "物体";
    return "";
}

/* ================================================================
 * 领域 → UI 事件（不直接调用 UI）
 * ================================================================ */
static void ev_publish(event_type_t type, event_prio_t prio, int source, int code,
                       const char *text, const char *loc, const char *level)
{
    system_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = type;
    ev.prio = prio;
    ev.source = source;
    ev.code = code;
    if (text)  snprintf(ev.text, sizeof(ev.text), "%s", text);
    if (loc)   snprintf(ev.loc, sizeof(ev.loc), "%s", loc);
    if (level) snprintf(ev.level, sizeof(ev.level), "%s", level);
    event_bus_publish(&ev);
}

static void ev_log(const char *title, const char *loc, const char *level)
{
    ev_publish(EV_SYS, EV_PRIO_P4, 0, 2, title, loc, level);
}

static void ev_toast(const char *text)
{
    ev_publish(EV_SYS, EV_PRIO_P4, 0, 3, text, NULL, NULL);
}

static void ev_user_activity(void)
{
    ev_publish(EV_SYS, EV_PRIO_P4, 0, 4, NULL, NULL, NULL);
}

static int camera_zone(void)
{
    int z = config_get()->camera_zone;
    if (z < 0 || z >= 4)
        z = 0;
    return z;
}

/* ================================================================
 * T5：告警统一入口
 *   1) 驱动 alarm_fsm；
 *   2) 发布 EV_ALARM 到 event_bus；
 *   3) UI 的 dispatch 定时器统一渲染。
 * ================================================================ */
static void bridge_alarm_trigger(int zone, const char *src)
{
    alarm_fsm_dispatch(zone, ALARM_EV_TRIGGER, src);
    alarm_fsm_dispatch(zone, ALARM_EV_CONFIRM, src);

    system_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = EV_ALARM;
    ev.prio = EV_PRIO_P1;
    ev.source = zone;
    ev.code = 1;
    snprintf(ev.text, sizeof(ev.text), "%s", src ? src : "");
    event_bus_publish(&ev);
}

/* C1/C5：告警入口。融合开启时登记摄像头事件并推迟定案；关闭时保持即时告警。 */
static void alarm_or_fuse(int zone, const char *src, int fuse_src)
{
    if (g_cfg.fusion_on) {
        fusion_add(zone, fuse_src, now_ms32(), 55);
        LOGD("FUSE", "zone=%d %s event queued", zone, src ? src : "?");
    } else {
        bridge_alarm_trigger(zone, src);
    }
}

static void bridge_alarm_clear(int zone, const char *reason)
{
    alarm_fsm_dispatch(zone, ALARM_EV_CLEAR, reason);

    system_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = EV_ALARM;
    ev.prio = EV_PRIO_P1;
    ev.source = zone;
    ev.code = 0;
    snprintf(ev.text, sizeof(ev.text), "%s", reason ? reason : "");
    event_bus_publish(&ev);
}

/* T10 配置同步等用到单调时钟 */
static uint32_t now_ms32(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000ULL +
                      (uint64_t)(ts.tv_nsec / 1000000L));
}

/* ================================================================
 * 主线程轮询：消费共享状态 → 告警事件 + 外设控制
 * 由 UI 的 200ms 定时器调用（本层不依赖 LVGL）。
 * ================================================================ */
void dev_bridge_poll(void)
{
    /* T8：桥接定时器在主线程运行；心跳停止说明 LVGL 主循环卡死 */
    health_beat(HEALTH_BRIDGE);

    /* T10：每 1s 从持久化配置同步一次（远程 set_config/set_warn 无需重启） */
    static int cfg_sync = 0;
    if (++cfg_sync >= 5) {
        cfg_sync = 0;
        const struct app_config *c = config_get();
        g_cfg = c->warn;
        sensor_pipeline_set_config(&g_cfg);
        sensor_pipeline_set_ap_calib(c->ap_ps_off, c->ap_ir_off,
                                     c->ap_als_scale ? c->ap_als_scale : 1000);
        cam_feed_set_motion_interval(c->motion_interval);
        cam_feed_set_motion_diff(c->motion_diff);
    }

    /* 采集线程事件（读取即清零） */
    int key_toggle = sensor_pipeline_take_key_toggle();
    int calib_done = sensor_pipeline_take_calib_done();
    int warn_hit[4], warn_pending[4];
    for (int i = 0; i < 4; i++) {
        warn_hit[i] = sensor_pipeline_take_warn(i);
        warn_pending[i] = sensor_pipeline_take_warn_pending(i);
    }

    /* --- KEY0：屏幕唤醒（重置 UI 空闲计时 + 解除 blank + 事件） --- */
    if (key_toggle > 0) {
        ev_user_activity();   /* 统一入口：g_idle_sec=0 + blank 0 */
        ev_log("KEY 触发", "系统", "");
    }

    /* --- 零偏校准完成（T3-C，采集线程置标志，主线程上报） --- */
    if (calib_done > 0) {
        ev_log("ICM20608 CAL OK", "系统", "");
        LOGI("CALIB", "zero bias reported to UI");
    }

    /* --- 传感器数据质量事件（T3-A：故障/恢复/数据陈旧，状态变化才上报） --- */
    sensor_mgr_check_stale();
    {
        sensor_status_t st;
        char nm[16];
        while (sensor_mgr_take_fault(NULL, &st, nm, sizeof(nm))) {
            if (st == SENSOR_OK) {
                char t[32];
                snprintf(t, sizeof(t), "%s 正常", nm);
                ev_log(t, "系统", "");
                LOGI("SENSOR", "%s recovered", nm);
            } else {
                char t[32];
                snprintf(t, sizeof(t), "%s 异常", nm);
                ev_log(t, "系统", "medium");
                LOGW("SENSOR", "%s fault: %s", nm, sensor_status_name(st));
            }
        }
    }

    /* --- C2 双阈值：预警轻提醒（不触发告警状态机/声光，带冷却） --- */
    time_t now = time(NULL);
    for (int i = 0; i < 4; i++) {
        if (warn_pending[i] <= 0) continue;
        if (!zone_state_is_armed(i)) continue;
        if (now - g_zone_last_warn[i] < g_cfg.cooldown_sec) continue;
        g_zone_last_warn[i] = now;
        char t[48];
        snprintf(t, sizeof(t), "%s 传感器预警", zone_state_name(i));
        ev_toast(t);
        ev_log(t, "预警", "low");
        LOGI("BRIDGE", "zone=%d pre-warn (below alarm threshold)", i);
    }

    /* --- 各位置告警触发（该分区布防中才生效，带消警冷却） --- */
    for (int i = 0; i < 4; i++) {
        if (warn_hit[i] <= 0) continue;
        if (!zone_state_is_armed(i)) continue;   /* 分区级布防门槛 */
        /* 冷却：距上次触发 < cooldown_sec 则忽略本次 */
        if (now - g_zone_last_trigger[i] < g_cfg.cooldown_sec) continue;
        g_zone_last_trigger[i] = now;
        bridge_alarm_trigger(i, "SENSOR");
    }

    /* --- 运动粗判命中（当前预览通道对应分区，布防中+冷却） ---
     * 单摄分时切换语义：运动发生在当前预览通道，告警归属其对应分区。
     * 统一入精判管线（08-30 定案）：detector 两段式（本地 SCRFD 初判 →
     * 云端复核定案），结论经下方轮询分发 */
    int mh = cam_feed_get_motion_hits();
    if (mh > 0) {
        int z = camera_zone();
        if (z >= 0 && zone_state_is_armed(z)
            && now - g_zone_last_trigger[z] >= g_cfg.cooldown_sec) {
            g_zone_last_trigger[z] = now;
            detector_submit(z);
        }
    }

    /* --- 精判结论轮询（统一两级管线，08-30 定案；线程 4 异步产出 200ms 内下发） ---
     * 复核关（纯本地）：worker 仅发 INITIAL——检出人脸→STRANGER / 未检出→INTRUDER（不漏报）
     * 复核开（两级）：
     *   INITIAL: 检出人脸→立即 STRANGER 告警；未检出→"画面变动"轻提醒（等云端定论）
     *   FINAL:   白名单命中→在告警则自动消警；陌生人→不在告警则升级 STRANGER；
     *            非人员（动物/物体/无人）→在告警则自动消警（本地误报），否则仅时间轴；
     *            不可达→初步=未检出则升级 INTRUDER（不漏报），已告警则维持本地结论
     * 自动消警只作用于结论分区（SENSOR 触发的其他分区告警不受影响） */
    {
        int rz = -1, rr = DET_RESULT_NONE, rst = DET_STAGE_INITIAL;
        char name[32];
        int take = detector_poll_result(&rz, &rr, &rst, name, sizeof(name)) && rz >= 0;

        /* 门槛：初步结论仅布防中处理；最终结论在布防中/告警中都处理——
         * 初判告警后分区已转 ZONE_ALARM（非 ARMED），若按布防门槛拦截，
         * 云端白名单/无人结论永远到不了自动消警逻辑（08-30 板测定位）；
         * 撤防（OFFLINE）后两种结论都丢弃 */
        if (take && rst == DET_STAGE_INITIAL && !zone_state_is_armed(rz))
            take = 0;
        if (take && rst == DET_STAGE_FINAL
            && !zone_state_is_armed(rz) && !zone_state_has_alarm(rz))
            take = 0;

        if (take) {
            if (rst == DET_STAGE_INITIAL) {
                g_pre_result = rr;   /* 暂存初步结论（FINAL 定案时参考） */
                if (rr == DET_RESULT_FACE_UNKNOWN) {
                    alarm_or_fuse(rz, "STRANGER", FUSE_SRC_CAM);   /* 融合开启则登记待定案 */
                } else if (rr == DET_RESULT_NO_FACE) {
                    if (config_get()->cloud_review) {
                        /* 轻提醒不打扰：未检出人脸（可能是光影，也可能人背对镜头），等云端定论 */
                        ev_toast("画面变动");
                        ev_log("画面变动（未检出人员）", "本地初判", "low");
                    } else {
                        alarm_or_fuse(rz, "INTRUDER", FUSE_SRC_CAM);
                    }
                }
            } else {   /* DET_STAGE_FINAL：云端复核定案 */
                const char *tw = det_type_word(name);
                switch (rr) {
                case DET_RESULT_FACE_KNOWN: {   /* 白名单命中：误告警自动撤销 */
                    if (zone_state_has_alarm(rz))
                        bridge_alarm_clear(rz, "云端复核：已授权人员");
                    else
                        ev_log("云端复核：已授权人员", "云端复核", "low");
                    break;
                }
                case DET_RESULT_FACE_UNKNOWN: { /* 云端陌生人：不在告警则升级（如背身人形） */
                    if (!zone_state_has_alarm(rz))
                        alarm_or_fuse(rz, "STRANGER", FUSE_SRC_CAM);
                    break;
                }
                case DET_RESULT_OTHER: {        /* 云端判定非人员：本地误报则撤销 */
                    char t[64];
                    if (tw[0])
                        snprintf(t, sizeof(t), "云端复核：判定为%s活动", tw);
                    else
                        snprintf(t, sizeof(t), "云端复核：判定无人员");
                    if (zone_state_has_alarm(rz))
                        bridge_alarm_clear(rz, t);
                    else
                        ev_log(t, "云端复核", "low");
                    break;
                }
                default: {                      /* 云端不可达：维持/升级本地结论（fail-safe） */
                    if (g_pre_result == DET_RESULT_NO_FACE && !zone_state_has_alarm(rz))
                        alarm_or_fuse(rz, "INTRUDER", FUSE_SRC_CAM);
                    break;
                }
                }
            }
        }
    }

    /* --- C1/C5：多源融合定案（fusion_on=1 时启作用） ---
     * 摄像头与传感器事件在时间窗内共现 → 置信度达标 → 告警；
     * 单源事件窗口过期仍未达阈值 → 只记轻事件，不告警。 */
    if (g_cfg.fusion_on) {
        uint32_t win = (g_cfg.fusion_window_ms > 0)
                           ? (uint32_t)g_cfg.fusion_window_ms : 3000u;
        int fz, fsc;
        while ((fz = fusion_poll(now_ms32(), win, g_cfg.fusion_threshold, &fsc)) >= 0) {
            if (!zone_state_is_armed(fz) && !zone_state_has_alarm(fz))
                continue;   /* 未布防：直接丢弃 */

            if (fsc >= g_cfg.fusion_threshold &&
                now - g_zone_last_trigger[fz] >= g_cfg.cooldown_sec) {
                g_zone_last_trigger[fz] = now;
                LOGI("FUSE", "zone=%d score=%d -> ALARM", fz, fsc);
                bridge_alarm_trigger(fz, "FUSION");
            } else {
                LOGI("FUSE", "zone=%d score=%d -> single-source", fz, fsc);
                ev_log("单源事件（未达融合阈值）", "融合判定", "low");
            }
        }
    }

    /* 事件总线由 UI 的 dispatch 定时器统一消费（本层只负责发布） */

    /* --- 告警分级声光（策略在 domain/alarm/warn_policy，输出在 infra/hal_io） ---
     * 级别：3=高（窗/周界）2=中（仓库）1=低（门）0=无告警 */
    warn_policy_step(&g_warn, zone_state_alarm_level(), zone_state_any_armed());

    if (hal_io_led_ok())
        hal_io_led_write(warn_policy_led(&g_warn));
    if (hal_io_beep_ok())
        hal_io_beep_write(warn_policy_beep(&g_warn));
}

/* ================================================================
 * 启动入口
 * ================================================================ */
void dev_bridge_start(void)
{
    if (g_started) return;
    g_started = 1;

    /* 从持久化配置恢复预警阈值（config 未初始化时在此兜底，兼容模拟器入口） */
    if (!config_path()[0])
        config_init(NULL);
    g_cfg = config_get()->warn;
    g_cfg_loaded = 1;
    {
        const struct app_config *c = config_get();
        sensor_pipeline_set_config(&g_cfg);
        sensor_pipeline_set_ap_calib(c->ap_ps_off, c->ap_ir_off,
                                     c->ap_als_scale ? c->ap_als_scale : 1000);
    }

    /* T5：事件总线 + 告警状态机 */
    event_bus_init();
    alarm_fsm_init();

    /* T3-A：传感器数据质量管理（滤波/异常剔除/故障状态） */
    sensor_mgr_init();
    /* T3-C：加载校准参数（config_init 已完成），可选上电零偏采集 */
    sensor_calib_init();

    if (getenv("GATEWAY_CALIB_ONCE")) {
        sensor_calib_start_zero_bias(2000);
        LOGI("CALIB", "GATEWAY_CALIB_ONCE set: keep device still for 2s");
    }

    /* 控制输出节点（失败仅禁用该外设，见 infra/hal_io） */
    warn_policy_reset(&g_warn);
    if (hal_io_open() != 0)
        LOGW("BRIDGE", "no output device available (LED/beep disabled)");

    /* 采集流水线（KEY0 + AP3216C + ICM20608，线程在 infra/sensor_pipeline） */
    if (sensor_pipeline_start() != 0) {
        LOGE("BRIDGE", "sensor pipeline start failed");
        return;
    }

    /* LVGL 主线程消费定时器（200ms） */
    /* 定时器已移到 UI 层（dev_bridge_poll），本层不再依赖 LVGL */
    detector_init();

    /* --- 功能初始化就绪上报（时间轴，开机一次） ---
     * 目的：在布防前让用户确认识别已开启；就绪=正常级别，未就绪=告警级别预警。
     * 本地粗判：开机预加载 SCRFD 模型（detector_model_ready），避免首次检测才加载——
     *   就绪 → "本地人脸识别 已就绪"；模型缺失 → 降级"只判有运动"（告警级）。
     * 云端复核：仅当"云端复核"开关开启时探测服务可达性——
     *   可达 → "云端复核 已就绪"；不可达 → "云端复核 不可达（本地兜底）"（告警级）。 */
    if (detector_model_ready() == 0)
        ev_log("本地人脸识别 已就绪", "本地初判", "");
    else
        ev_log("本地模型未加载（降级-只判有运动）", "本地初判", "medium");

    if (config_get()->cloud_review) {
        if (cloud_detect_ready() == 0)
            ev_log("云端复核 已就绪", "云端", "");
        else
            ev_log("云端复核 不可达（本地兜底）", "云端", "medium");
    }

    LOGI("BRIDGE", "device bridge started");
}

/* T5-B：停止采集/IMU 线程并 join（退出流程调用；采集线程 poll 上限 100ms，
 * IMU 线程 poll 上限 200ms，所以通常 <300ms 返回） */
void dev_bridge_stop(void)
{
    sensor_pipeline_stop();
    hal_io_close();

    LOGI("BRIDGE", "device bridge stopped");
}

/* ================================================================
 * 诊断快照获取（UI 检测面板用）
 * ================================================================ */
void dev_bridge_get_diag(struct dev_diag *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));

    sensor_pipeline_get_diag(out);


    out->led_ok   = hal_io_led_ok();
    out->beep_ok  = hal_io_beep_ok();
    out->pwm_ok   = hal_io_pwm_ok();
    out->pwm_on   = hal_io_pwm_state();
    out->pwm_freq = hal_io_pwm_ok() ? hal_io_pwm_freq() : 0;
    out->led_on   = (hal_io_led_state() == 1);
    out->beep_on  = (hal_io_beep_state() == 1);
}

/* ================================================================
 * 预警阈值配置（设置弹窗可调，采集线程实时生效）
 * ================================================================ */
void dev_bridge_get_config(struct dev_config *out)
{
    if (!out) return;
    /* UI 设置弹窗在 dev_bridge_start 之前构建：首次读取时从持久化配置恢复，
     * 保证重启后显示的是上次保存的阈值，而不是编译期默认值。 */
    if (!g_cfg_loaded && config_path()[0]) {
        g_cfg = config_get()->warn;
        g_cfg_loaded = 1;
    }
    *out = g_cfg;
}

void dev_bridge_set_config(const struct dev_config *cfg)
{
    if (!cfg) return;
    if (!g_cfg_loaded && config_path()[0]) {
        g_cfg = config_get()->warn;
        g_cfg_loaded = 1;
    }
    g_cfg = *cfg;   /* 整表替换（UI 侧先 get 再改再 set） */
    if (g_cfg.debounce < 1) g_cfg.debounce = 1;
    if (g_cfg.debounce > 20) g_cfg.debounce = 20;
    for (int i = 0; i < 4; i++) {
        if (g_cfg.zones[i].threshold < 0) g_cfg.zones[i].threshold = 0;
        if (g_cfg.zones[i].threshold > 2000) g_cfg.zones[i].threshold = 2000;
        if (g_cfg.zones[i].src < WARN_SRC_NONE) g_cfg.zones[i].src = WARN_SRC_NONE;
        if (g_cfg.zones[i].src > WARN_SRC_ICM) g_cfg.zones[i].src = WARN_SRC_ICM;
    }
    sensor_pipeline_set_config(&g_cfg);

    /* 持久化：只更新内存并置 dirty，由 UI 的 1s 定时器统一 flush，
     * 避免滑条拖动时频繁写盘（TF/rootfs 写放大与 UI 卡顿）。 */
    struct app_config ac = *config_get();
    ac.warn = g_cfg;
    config_update(&ac);
}

/* AP3216C 校准（环境基线 + 光照比例）：立即生效并持久化 */
void dev_bridge_set_ap_calib(int ps_off, int ir_off, int als_scale_x1000)
{
    if (als_scale_x1000 < 500) als_scale_x1000 = 500;
    if (als_scale_x1000 > 2000) als_scale_x1000 = 2000;
    if (ps_off < -2048) ps_off = -2048;
    if (ps_off > 2048) ps_off = 2048;
    if (ir_off < -2048) ir_off = -2048;
    if (ir_off > 2048) ir_off = 2048;

    sensor_pipeline_set_ap_calib(ps_off, ir_off, als_scale_x1000);

    struct app_config ac = *config_get();
    ac.ap_ps_off = ps_off;
    ac.ap_ir_off = ir_off;
    ac.ap_als_scale = als_scale_x1000;
    config_update(&ac);
    config_flush();
    LOGI("BRIDGE", "AP3216C calib: ps_off=%d ir_off=%d als_scale=%d",
         ps_off, ir_off, als_scale_x1000);
}

void dev_bridge_get_ap_calib(int *ps_off, int *ir_off, int *als_scale_x1000)
{
    sensor_pipeline_get_ap_calib(ps_off, ir_off, als_scale_x1000);
}

int dev_bridge_get_cooldown_remain(int zone)
{
    if (zone < 0 || zone >= 4)
        return 0;
    int cd = g_cfg.cooldown_sec;
    if (cd <= 0)
        return 0;
    time_t now = time(NULL);
    int elapsed = (int)(now - g_zone_last_trigger[zone]);
    if (elapsed < 0)
        elapsed = 0;
    int remain = cd - elapsed;
    return remain > 0 ? remain : 0;
}

/* ================================================================
 * 测试面板手动控制
 * ================================================================ */

/* 手动开关板载蜂鸣器：锁定状态，直到再次点击（告警时自动让位） */
void dev_bridge_set_beep(int on)
{
    warn_policy_set_beep_manual(&g_warn, on);
    hal_io_beep_write(on);
}

/* 手动开关板载 LED：锁定状态，直到再次点击（告警时自动让位） */
void dev_bridge_set_led(int on)
{
    warn_policy_set_led_manual(&g_warn, on);
    hal_io_led_write(on);
}

/* 手动开关 PWM 蜂鸣器并设置频率（beep_pwm ioctl） */
void dev_bridge_set_pwm(int on, int freq)
{
    if (!hal_io_pwm_ok()) return;
    /* 手动调速默认 30% 占空比：
     * 50% 时无源蜂鸣器电流脉冲最大，会通过电源/振动干扰 OV5640，
     * 表现为抓拍照片出现行错位（不是显示撕裂）。 */
    hal_io_pwm_write(on, freq, 30);
}

void dev_bridge_set_beep_mute(int mute)
{
    warn_policy_set_mute(&g_warn, mute);
    if (hal_io_beep_ok())
        hal_io_beep_write(warn_policy_beep(&g_warn));   /* 静音立即生效 */
}

void dev_bridge_set_als_en(int en)
{
    sensor_pipeline_set_als_en(en);
}

void dev_bridge_set_imu_en(int en)
{
    sensor_pipeline_set_imu_en(en);
}

int dev_bridge_get_als_en(void)
{
    return sensor_pipeline_get_als_en();
}

int dev_bridge_get_imu_en(void)
{
    return sensor_pipeline_get_imu_en();
}
