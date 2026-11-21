/* ================================================================
 * ui_home.c — 1024x600 中文安防网关主页（完全重写）
 *
 * 布局: flex-col 1024x600
 *   status-bar 36px
 *   main-area  flex:1(512px) flex-row (left 630px | right 360px)
 *   bottom-bar 52px
 *
 * 内存预算:
 *   LV_MEM_SIZE = 256KB
 *   LVGL 对象 < 200
 *   定时器 = 2（时钟 + 呼吸/扫描线）
 *   事件列表 = 10 条（静态数组预分配）
 * ================================================================ */
#include "ui_home.h"
#include "ui_widgets.h"
#include "ui_events.h"
#include "ui_theme.h"
#include "ui_terminal.h"
#include "ui_internal.h"
#include "panels/ui_status_bar.h"
#include "panels/ui_timeline.h"
#include "modals/ui_alarm.h"
#include "modals/ui_detail.h"
#include "modals/ui_settings.h"
#include "modals/ui_album.h"
#include "panels/ui_sensor.h"
#include "ui_canvas.h"
#include "ui_time.h"
#include "../zone_types.h"
#include "../domain/zone_state.h"
#include "../event_bus.h"
#include "../dev_bridge.h"
#include "../cam_feed.h"
#include "../detector.h"
#include "../storage_mgr.h"
#include "../mqtt_hub.h"
#include "../config_mgr.h"
#include "../sensor_calib.h"
#include "../log_svc.h"
#include "../health_mgr.h"
#include "../lvgl_port.h"
#include "../infra/net_probe.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <stdint.h>
#include <signal.h>
#include <sys/wait.h>
#include <time.h>
#include <sys/time.h>
#include <sys/statvfs.h>

/* ---- 主题（字库 + 色板）：ui_theme.h 统一提供，子模块共用 ---- */

/* ================================================================
 * 布局尺寸常量
 * ================================================================ */
#define SCR_W        UI_SCR_W
#define SCR_H        UI_SCR_H
#define SCR_VIS_H    572   /* 面板底部物理遮挡约28px，可视区高度（底栏完整显示在安全区内） */
#define STATUS_H     UI_STATUS_H
#define BOTTOM_H     52
/* 主区高度 = 可视572 - 状态栏36 - 底栏52 = 484（info-bar 已删除，空间补偿给主区） */
#define MAIN_H       (SCR_VIS_H - STATUS_H - BOTTOM_H)
#define LEFT_W       630
#define RIGHT_W      360
#define CAM_H        396   /* 分区卡片改一行后，摄像头预览区大幅加大 */
/* 摄像头头部两行：第一行徽标+通道信息，第二行切换 pill，各 28px */
#define CAM_HDR1_H   28
#define CAM_HDR2_H   28
#define CAM_PIC_Y    (CAM_HDR1_H + CAM_HDR2_H) /* 预览画面区起始 y */
#define GAP_SM       6
#define GAP_MD       8

/* ================================================================
 * 分区数据（类型与表在 ui_internal.h 中声明，供 ui_events 共享）
 * ================================================================ */
const zone_info_t zone_info[ZONE_COUNT] = {
    {"前门", "Z-001", "PIR+门",   "192.168.1.101", "v2.3.1", "-42dBm"},
    {"后门", "Z-002", "PIR+门",   "192.168.1.102", "v2.3.1", "-55dBm"},
    {"窗户", "Z-003", "门+窗",     "192.168.1.103", "v2.3.0", "-38dBm"},
    {"仓库", "Z-004", "PIR+监",   "192.168.1.104", "v2.3.1", "-61dBm"},
};

/* ================================================================
 * 摄像头：单路固定安装（不做多通道切换）
 * 摄像头只服务一个防区，归属由 config.camera_zone 决定。
 * ================================================================ */

/* ================================================================
 * 全局状态
 * ================================================================ */
/* 分区状态统一由 domain/zone_state 持有，UI 不再维护副本。 */

/* ================================================================
 * 需要动态更新的 LVGL 对象指针
 * ================================================================ */
static lv_obj_t *g_cam_title_lbl   = NULL;
static lv_obj_t *g_cam_zone_lbl    = NULL;   /* “单摄预览 · 防区：X” */
static lv_obj_t *g_cam_text_lbl    = NULL;
static lv_obj_t *g_zone_cards[ZONE_COUNT];
static lv_obj_t *g_zones_grid = NULL;      /* 单防区卡片容器 */
int       g_zone_card_idx = -1;     /* 当前展示的防区索引 */
static lv_obj_t *g_zone_cool_lbl = NULL;   /* 冷却剩余时间 */
static lv_obj_t *g_zone_status[ZONE_COUNT];
static lv_obj_t *g_zone_dots[ZONE_COUNT];
static lv_obj_t *g_scanline        = NULL;
static lv_obj_t *g_cam_grid_lines[32];   /* 网格线句柄（有画面隐藏，无画面显示） */
static int       g_cam_grid_cnt    = 0;
static int       g_cam_overlay_hidden = 0;   /* 1=占位文字/网格已隐藏（有画面） */
static lv_obj_t *g_toast           = NULL;
/* 详情弹窗：实现见 ui/modals/ui_detail.c */
static lv_obj_t *g_detail_body     = NULL;
static lv_obj_t *g_detail_title_lbl= NULL;  /* 详情弹窗标题标签（动态更新分区名） */
static lv_obj_t *g_detail_state_lbl= NULL;  /* 详情弹窗"当前状态"值标签 */
static lv_obj_t *g_detail_arm_btn  = NULL;  /* 详情弹窗状态设置按钮（布防⇄撤防） */
static int g_detail_zone_idx       = -1;    /* 详情弹窗当前显示的分区索引 */
static lv_obj_t *g_cam_wrap        = NULL;  /* 摄像头容器（切换时改背景色） */

/* 传感器检测面板（卡片网格 + 子页） */



/* 曲线数据（UI 线程独占） */

/* 系统页指标/日志 */

/* 曲线坐标轴（左侧 Y 刻度 + 底部时间刻度） */

/* 主界面迷你面板 */
static lv_obj_t *g_mini_lbl[4] = { NULL, NULL, NULL, NULL };
static lv_obj_t *g_mini_chart  = NULL;
static lv_chart_series_t *g_mini_ser = NULL;

int       g_idle_sec = 0;            /* 无触摸空闲秒数（超时休眠） */
#define IDLE_SLEEP_SEC  600                 /* 10 分钟无操作 → 屏幕熄灭 */

/* 告警确认弹窗：实现见 ui/modals/ui_alarm.c（本文件只负责接线 hooks） */

/* 摄像头画布（630×340 铺满画面区，静态缓冲 ≈428KB 不占 LVGL 堆；UI 定时器从 cam_feed 双缓冲 blit） */
static uint8_t g_cam_canvas_buf[CAM_DISP_W * CAM_DISP_H * 2];
static lv_obj_t *g_cam_canvas = NULL;       /* canvas 对象 */

/* 云端复核开关（08-30 统一两级管线：本地初判立即响应 + 云端复核定案/白名单自动消警）
 * 宏与状态在 ui_internal.h 中声明（供 ui_events.c 共享） */
int g_cloud_review = CLOUD_REVIEW_ON;    /* 默认开（两级管线） */
static lv_obj_t *g_mode_badge_lbl = NULL;       /* 摄像头区复核状态徽章（联动） */

/* ================================================================
 * 抓拍存储（真实画面）：全图（查看弹窗）+ 缩略图（相册 3×2）
 * 触发：抓拍按钮（手动）/ 预警触发（自动）；超出 6 张环形覆盖最旧
 * 内存：全图 6×428KB + 缩略图 6×65KB ≈ 2.96MB（静态分配，不占 LVGL 堆）
 * ================================================================ */



/* 设置弹窗动态控件 */
int g_brightness = 80;               /* HTML 初始 80% */
int g_schedule_on = 0;               /* 定时布防开关状态 */
int g_sched_start_h = 22;           /* 定时布防开始小时（默认 22:00） */
int g_sched_end_h   = 6;            /* 定时布防结束小时（默认 06:00） */

/* ================================================================
 * 定时器（仅2个）
 * ================================================================ */
static lv_timer_t *g_timer_clock = NULL;
static lv_timer_t *g_timer_anim  = NULL;
static int g_scan_y = 0;
static int g_breath_phase = 0;

/* 时间字符串（HH:MM）由 ui/ui_time 提供，时钟定时器每秒同步当日秒数 */

/* ================================================================
 * 前向声明
 * ================================================================ */

/* ================================================================
 * 前向声明（本段函数互相调用）
 * ================================================================ */
static void _build_cam_wrap(lv_obj_t *parent);
static void _build_zones_grid(lv_obj_t *parent);
static void _on_restart_app(lv_event_t *e);
static void _build_right_col(lv_obj_t *parent);
static void _build_bottom_bar(lv_obj_t *parent);
static void _cam_refresh_cb(lv_timer_t *timer);
static void _on_arm(lv_event_t *e);
static void _on_disarm(lv_event_t *e);
static void _on_silence(lv_event_t *e);
static void _on_capture(lv_event_t *e);
static void _on_test(lv_event_t *e);
static void _on_screen_touch(lv_event_t *e);
static int  _any_modal_visible(void);
static void _timer_clock_cb(lv_timer_t *t);
static void _timer_anim_cb(lv_timer_t *t);
static void net_timer_cb(lv_timer_t *t);
static void dev_poll_cb(lv_timer_t *t);
static void ui_events_dispatch_cb(lv_timer_t *t);

static int g_screen_blanked = 0;   /* 息屏是否已 blank（0=亮） */
static int g_cam_recover_frames = 0;  /* 唤醒后强制放行摄像头帧数（约 3s） */
static void _toast_dismiss_cb(lv_timer_t *t)
{
    /* v9 中 lv_timer_t 为不透明结构体，不能直接访问 t->user_data，
     * 改用文件内全局指针 g_toast 获取 toast 对象 */
    (void)t;
    if (g_toast) lv_obj_add_flag(g_toast, LV_OBJ_FLAG_HIDDEN);
    lv_timer_del(t);
}

void _show_toast(const char *txt)
{
    if (!g_toast || !txt) return;
    lv_label_set_text(g_toast, txt);
    lv_obj_clear_flag(g_toast, LV_OBJ_FLAG_HIDDEN);
    lv_timer_t *t = lv_timer_create(_toast_dismiss_cb, 1500, g_toast);
    if (t) lv_timer_set_repeat_count(t, 1);
}

void _update_zone_visual(int i)
{
    if (i < 0 || i >= ZONE_COUNT) return;
    zone_state_t st = zone_state_get(i);
    lv_obj_t *card = g_zone_cards[i];
    lv_obj_t *dot  = g_zone_dots[i];
    lv_obj_t *stat = g_zone_status[i];

    if (!card) return;

    switch (st) {
    case ZONE_ARMED:
        lv_obj_set_style_border_color(card, lv_color_hex(CLR_GREEN), 0);
        if (dot) lv_obj_set_style_bg_color(dot, lv_color_hex(CLR_GREEN), 0);
        if (stat) lv_label_set_text(stat, "布防中");
        break;
    case ZONE_ALARM:
        lv_obj_set_style_border_color(card, lv_color_hex(CLR_RED), 0);
        if (dot) lv_obj_set_style_bg_color(dot, lv_color_hex(CLR_RED), 0);
        if (stat) lv_label_set_text(stat, "异常触发");
        break;
    case ZONE_OFFLINE:
        lv_obj_set_style_border_color(card, lv_color_hex(CLR_TEXT_DIM), 0);
        if (dot) lv_obj_set_style_bg_color(dot, lv_color_hex(CLR_TEXT_DIM), 0);
        if (stat) lv_label_set_text(stat, "离线");
        break;
    default: /* ZONE_ONLINE */
        lv_obj_set_style_border_color(card, lv_color_hex(CLR_BORDER), 0);
        if (dot) lv_obj_set_style_bg_color(dot, lv_color_hex(CLR_GREEN), 0);
        if (stat) lv_label_set_text(stat, "已撤防");
        break;
    }

    ui_status_bar_refresh_arm();   /* 状态栏布防指示联动（任一布防=布防中） */
}

static void _on_arm(lv_event_t *e)
{
    (void)e;
    zone_state_arm_all();   /* 全量布防（幂等）：部分布防状态下可补齐其余分区 */

    for (int i = 0; i < ZONE_COUNT; i++) {
        zone_state_set(i, ZONE_ARMED);
        _update_zone_visual(i);   /* 状态栏指示由 _update_arm_indicator 联动 */
    }

    _show_toast("全部分区已布防");
    _add_event("全部分区已布防",
               "系统", CLR_GREEN, "");   /* HTML addEvent('ok',...) */
}

static void _on_disarm(lv_event_t *e)
{
    (void)e;
    if (!ui_events_zone_has_armed()) return;
    zone_state_disarm_all();

    for (int i = 0; i < ZONE_COUNT; i++) {
        zone_state_set(i, ZONE_ONLINE);
        _update_zone_visual(i);
    }

    _show_toast("全部分区已撤防");
    _add_event("全部分区已撤防",
               "系统", CLR_AMBER, "");   /* HTML addEvent('warn',...) */
}

static void _on_silence(lv_event_t *e)
{
    (void)e;
    int had_alarm = 0;
    for (int i = 0; i < ZONE_COUNT; i++) {
        if (zone_state_get(i) == ZONE_ALARM) {
            zone_state_ack(i);
            /* 告警必产生于布防状态，消除后回到布防中 */
            zone_state_set(i, ZONE_ARMED);
            _update_zone_visual(i);
            had_alarm = 1;
        }
    }
    if (had_alarm) {
        _show_toast("报警已消");
        _add_event("报警已消",
                   "系统", CLR_GREEN, "");   /* HTML addEvent('ok',...) */
    } else {
        _show_toast("当前状态正常");
        _add_event("当前正常",
                   "系统", CLR_CYAN, "");    /* HTML addEvent('info',...) */
    }
}

/* ================================================================
 * 回调：设置按钮 — 显示设置弹窗
 * ================================================================ */

void _screen_set_blank(int on)
{
    int fd = open("/sys/class/graphics/fb0/blank", O_WRONLY);
    if (fd < 0) return;
    if (lseek(fd, 0, SEEK_SET) >= 0) {
        char c = on ? '1' : '0';
        if (write(fd, &c, 1) < 0) LOGE("UI", "blank write: %s", strerror(errno));
    }
    close(fd);
    g_screen_blanked = on ? 1 : 0;

    if (!on) {
        /* 唤醒：整屏 + 摄像头画布强制重绘，避免恢复瞬间显示旧帧/撕裂 */
        lv_obj_invalidate(lv_screen_active());
        if (g_cam_canvas) lv_obj_invalidate(g_cam_canvas);

        /* 关键：blank 期间 fbdev 的 VSYNC 状态可能处于“最近一次失败”，
         * 若摄像头帧继续被 vsync_ok() 拦住，就永远不会触发 flush 去恢复，
         * 形成自锁（表现为画面卡在息屏前最后一帧）。
         * 唤醒后 3 秒内强制放行摄像头帧，让 flush 重新锁相。 */
        g_cam_recover_frames = 90;   /* 33ms × 90 ≈ 3s */
    }
}

static void _on_screen_touch(lv_event_t *e)
{
    (void)e;
    ui_events_user_activity();
}

static void _on_test(lv_event_t *e)
{
    (void)e;
    ui_sensor_open();
}

static void _on_capture(lv_event_t *e)
{
    (void)e;
    int z = ui_events_current_cam_zone();
    const char *zn = ui_events_zone_name(z);
    ui_album_capture_current(zn, "");
    _show_toast("抓拍已保存");
    _add_event("手动抓拍已保存", zn[0] ? zn : "摄像头", CLR_GREEN, "");
}

static int _any_modal_visible(void)
{
    if (ui_detail_visible()) return 1;
    if (ui_settings_visible()) return 1;
    if (ui_album_visible()) return 1;
    if (ui_album_viewer_visible()) return 1;
    if (ui_sensor_visible()) return 1;
    if (ui_alarm_visible()) return 1;
    return 0;
}

static void _timer_clock_cb(lv_timer_t *timer)
{
    (void)timer;

    /* 真实时间：读系统时钟（板上 date/NTP 同步后自动跟随） */
    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);

    /* 事件时间戳来源同步为真实 HH:MM（g_sim_sec 换算为当日秒偏移） */
    ui_time_set_day_seconds(tm_now.tm_hour * 3600 + tm_now.tm_min * 60 + tm_now.tm_sec);

    /* 摄像头标题：单摄 + 实测帧率（每秒刷新） */
    if (g_cam_title_lbl && g_cam_canvas) {
        char b[32];
        snprintf(b, sizeof(b), "摄像头 | %dfps", cam_feed_get_fps());
        lv_label_set_text(g_cam_title_lbl, b);
    }

    /* 摄像头归属防区变化时刷新文案（set_config 下发后无需重启） */
    {
        static int last_cam_zone = -1;
        int cz = config_get()->camera_zone;
        if (g_cam_zone_lbl && cz != last_cam_zone) {
            char zt[48];
            snprintf(zt, sizeof(zt), "摄像头 | 区域: %s", ui_events_zone_name(cz));
            lv_label_set_text(g_cam_zone_lbl, zt);
            last_cam_zone = cz;
        }
    }

    /* 防区卡右侧：告警触发后的冷却倒计时 */
    if (g_zone_cool_lbl) {
        int cz = g_zone_card_idx;
        if (cz < 0) cz = config_get()->camera_zone;
        int rem = dev_bridge_get_cooldown_remain(cz);
        if (rem > 0) {
            char b[32];
            snprintf(b, sizeof(b), "冷却 %ds", rem);
            lv_label_set_text(g_zone_cool_lbl, b);
            lv_obj_set_style_text_color(g_zone_cool_lbl, lv_color_hex(CLR_AMBER), 0);
        } else {
            lv_label_set_text(g_zone_cool_lbl, "区域由云端配置");
            lv_obj_set_style_text_color(g_zone_cool_lbl, lv_color_hex(CLR_TEXT_DIM), 0);
        }
    }

    /* 定时布防自动执行：开关开启且当前小时在 [start, end) 区间内 → 布防 */
    if (g_schedule_on) {
        int in_window;
        if (g_sched_start_h <= g_sched_end_h)
            in_window = (tm_now.tm_hour >= g_sched_start_h && tm_now.tm_hour < g_sched_end_h);
        else   /* 跨天：22:00-06:00 */
            in_window = (tm_now.tm_hour >= g_sched_start_h || tm_now.tm_hour < g_sched_end_h);
        if (in_window && !ui_events_is_armed()) {
            for (int i = 0; i < ZONE_COUNT; i++)
                ui_events_zone_set_state(i, ZONE_ARMED);
            ui_events_log("全部分区已布防", "系统", "");
            _show_toast("定时布防");
        } else if (!in_window && ui_events_is_armed()) {
            for (int i = 0; i < ZONE_COUNT; i++)
                ui_events_zone_set_state(i, ZONE_ONLINE);
            ui_events_log("全部分区已撤防", "系统", "");
        }
    }

    /* 测试模块弹窗打开时：每秒刷新实时数据 */
    if (ui_sensor_visible())
        ui_sensor_tick_1s();

    /* 空闲休眠：60s 无触摸 → fb blank；触摸/KEY0 唤醒（blank 0） */
    g_idle_sec++;
    if (g_idle_sec >= IDLE_SLEEP_SEC) {
        _screen_set_blank(1);
        g_idle_sec = IDLE_SLEEP_SEC;   /* 饱和，避免反复写 */
    }

    /* 弹窗打开期间不刷新时钟文字（避免状态栏局部重绘穿帮），关闭后自动补上 */
    if (_any_modal_visible()) return;
    if (g_screen_blanked) return;   /* 休眠期间不绘制 */

    ui_status_bar_update_clock();

}

static void _timer_anim_cb(lv_timer_t *timer)
{
    (void)timer;

    /* 屏幕休眠期间冻结动画（避免无效绘制 + 唤醒撕裂） */
    if (g_screen_blanked) return;

    /* 弹窗打开期间冻结动画（扫描线/呼吸），关闭后自动恢复 */
    if (_any_modal_visible()) return;

    g_breath_phase++;
    if (g_breath_phase > 100) g_breath_phase = 0;

    /* 呼吸动画：调整布防中卡片的 shadow_opa（任一分区布防即生效） */
    if (ui_events_zone_has_armed()) {
        /* sin-like: 0..40..0 */
        int opa;
        if (g_breath_phase <= 50)
            opa = g_breath_phase * 40 / 50;
        else
            opa = (100 - g_breath_phase) * 40 / 50;

        for (int i = 0; i < ZONE_COUNT; i++) {
            if (zone_state_get(i) == ZONE_ARMED && g_zone_cards[i]) {
                lv_obj_set_style_shadow_opa(g_zone_cards[i], opa, 0);
                lv_obj_set_style_shadow_width(g_zone_cards[i], 8 + opa / 4, 0);
            }
        }
    }

    /* 扫描线移动 */
    if (g_scanline) {
        g_scan_y += 2;
        if (g_scan_y > CAM_H) g_scan_y = 0;
        lv_obj_set_y(g_scanline, g_scan_y);
    }
}

static void _build_cam_wrap(lv_obj_t *par)
{
    lv_obj_t *cam = uiw_obj(par);
    if (!cam) return;
    g_cam_wrap = cam;
    lv_obj_set_size(cam, LEFT_W, CAM_H);
    lv_obj_set_style_bg_opa(cam, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(cam, lv_color_hex(0x003C50), 0);
    lv_obj_set_style_border_color(cam, lv_color_hex(CLR_BORDER), 0);
    lv_obj_set_style_border_width(cam, 1, 0);
    lv_obj_add_flag(cam, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_clear_flag(cam, LV_OBJ_FLAG_SCROLLABLE);

    /* ---- 摄像头头部第一行 cam-header：徽标居左 + 通道信息居右 ---- */
    lv_obj_t *hdr = uiw_obj(cam);
    if (hdr) {
        lv_obj_set_size(hdr, LEFT_W, CAM_HDR1_H);
        lv_obj_set_style_bg_opa(hdr, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(hdr, lv_color_hex(CLR_BLACK), 0);
        lv_obj_set_style_bg_opa(hdr, LV_OPA_40, 0);
        lv_obj_set_style_border_side(hdr, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_color(hdr, lv_color_hex(CLR_BORDER), 0);
        lv_obj_set_style_border_width(hdr, 1, 0);
        lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_SPACE_BETWEEN,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_left(hdr, 10, 0);
        lv_obj_set_style_pad_right(hdr, 10, 0);
        lv_obj_align(hdr, LV_ALIGN_TOP_MID, 0, 0);
        lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);

        /* mode-badge：云端复核状态徽章（设置页切换 开/关 后联动） */
        lv_obj_t *badge = uiw_obj(hdr);
        if (badge) {
            lv_obj_set_size(badge, LV_SIZE_CONTENT, 26);
                        lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(badge, lv_color_hex(CLR_CYAN), 0);
            lv_obj_set_style_bg_opa(badge, LV_OPA_20, 0);
            lv_obj_set_style_border_color(badge, lv_color_hex(CLR_CYAN), 0);
            lv_obj_set_style_border_width(badge, 1, 0);
            lv_obj_set_style_pad_left(badge, 8, 0);
            lv_obj_set_style_pad_right(badge, 8, 0);
            lv_obj_clear_flag(badge, LV_OBJ_FLAG_SCROLLABLE);
            /* 自适应宽度完整显示；label 指针保存供开关切换联动 */
            g_mode_badge_lbl = uiw_label(badge, "云端复核模式",
                      CLR_CYAN);
        }

        /* cam-title：单摄信息（fps 由 cam_feed 实测，每秒刷新） */
        g_cam_title_lbl = uiw_label_font(hdr, "摄像头 | 30fps",
                                          CLR_TEXT_DIM, FONT_UI);
    }

    /* ---- 摄像头画布（630×340 铺满画面区，最先创建=最底层；
     *      pill/网格/准星/扫描线/抓拍按钮全部在其上层，不被遮挡） ---- */
    {
        g_cam_canvas = lv_canvas_create(cam);
        if (g_cam_canvas) {
            memset(g_cam_canvas_buf, 0, sizeof(g_cam_canvas_buf));   /* 初始黑 */
            lv_canvas_set_buffer(g_cam_canvas, g_cam_canvas_buf,
                                 CAM_DISP_W, CAM_DISP_H, LV_COLOR_FORMAT_RGB565);
            lv_obj_set_pos(g_cam_canvas, 0, CAM_PIC_Y);   /* 铺满画面区 */
            lv_obj_clear_flag(g_cam_canvas, LV_OBJ_FLAG_SCROLLABLE);
        }
    }

    /* ---- 摄像头头部第二行：4 个通道切换 pill 水平居中 ---- */
    {
        lv_obj_t *sw = uiw_obj(cam);
        if (sw) {
            lv_obj_set_size(sw, LEFT_W, CAM_HDR2_H);
            lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(sw, lv_color_hex(CLR_BLACK), 0);
            lv_obj_set_style_bg_opa(sw, LV_OPA_40, 0);
            lv_obj_set_flex_flow(sw, LV_FLEX_FLOW_ROW);
            /* 主轴居中排列 pill */
            lv_obj_set_flex_align(sw, LV_FLEX_ALIGN_CENTER,
                                  LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_gap(sw, 8, 0);
            lv_obj_align(sw, LV_ALIGN_TOP_MID, 0, CAM_HDR1_H);
            lv_obj_clear_flag(sw, LV_OBJ_FLAG_SCROLLABLE);

            /* 单摄：不再提供 4 通道切换，只显示自身归属防区 */
            char zt[48];
            snprintf(zt, sizeof(zt), "摄像头 | 区域: %s",
                     ui_events_zone_name(config_get()->camera_zone));
            lv_obj_t *zl = uiw_label_font(sw, zt, CLR_TEXT_LO, FONT_UI);
            if (zl) {
                lv_obj_center(zl);
                g_cam_zone_lbl = zl;
            }
        }
    }

    /* ---- 网格线（用多个细长对象模拟，从两行头部下方开始；有画面时隐藏） ---- */
    /* 横线 */
    for (int y = CAM_PIC_Y; y < CAM_H; y += 48) {
        lv_obj_t *line = uiw_obj(cam);
        if (!line) continue;
        lv_obj_set_size(line, LEFT_W, 1);
        lv_obj_set_style_bg_opa(line, LV_OPA_20, 0);
        lv_obj_set_style_bg_color(line, lv_color_hex(CLR_BORDER), 0);
        lv_obj_align(line, LV_ALIGN_TOP_LEFT, 0, y);
        if (g_cam_grid_cnt < (int)(sizeof(g_cam_grid_lines) / sizeof(g_cam_grid_lines[0])))
            g_cam_grid_lines[g_cam_grid_cnt++] = line;
    }
    /* 竖线 */
    for (int x = 48; x < LEFT_W; x += 48) {
        lv_obj_t *line = uiw_obj(cam);
        if (!line) continue;
        lv_obj_set_size(line, 1, CAM_H);
        lv_obj_set_style_bg_opa(line, LV_OPA_20, 0);
        lv_obj_set_style_bg_color(line, lv_color_hex(CLR_BORDER), 0);
        lv_obj_align(line, LV_ALIGN_TOP_LEFT, x, 0);
        if (g_cam_grid_cnt < (int)(sizeof(g_cam_grid_lines) / sizeof(g_cam_grid_lines[0])))
            g_cam_grid_lines[g_cam_grid_cnt++] = line;
    }

    /* ---- 十字准星（居中于两行头部下方的画面区） ---- */
    {
        int pic_cy = (CAM_PIC_Y + CAM_H) / 2 - CAM_H / 2; /* 画面区中心相对 cam 中心的偏移 */
        lv_obj_t *ch_h = uiw_obj(cam);
        if (ch_h) {
            lv_obj_set_size(ch_h, 80, 1);
            lv_obj_set_style_bg_opa(ch_h, LV_OPA_50, 0);
            lv_obj_set_style_bg_color(ch_h, lv_color_hex(CLR_CYAN), 0);
            lv_obj_align(ch_h, LV_ALIGN_CENTER, 0, pic_cy);
        }
        lv_obj_t *ch_v = uiw_obj(cam);
        if (ch_v) {
            lv_obj_set_size(ch_v, 1, 80);
            lv_obj_set_style_bg_opa(ch_v, LV_OPA_50, 0);
            lv_obj_set_style_bg_color(ch_v, lv_color_hex(CLR_CYAN), 0);
            lv_obj_align(ch_v, LV_ALIGN_CENTER, 0, pic_cy);
        }
    }

    /* ---- 扫描线（全局引用，动画移动） ---- */
    g_scanline = uiw_obj(cam);
    if (g_scanline) {
        lv_obj_set_size(g_scanline, LEFT_W, 1);
        lv_obj_set_style_bg_opa(g_scanline, LV_OPA_30, 0);
        lv_obj_set_style_bg_color(g_scanline, lv_color_hex(CLR_CYAN), 0);
        lv_obj_align(g_scanline, LV_ALIGN_TOP_LEFT, 0, 0);
        lv_obj_add_flag(g_scanline, LV_OBJ_FLAG_EVENT_BUBBLE);
    }

    /* ---- 预览文字（单片画面，居中于画面区） ---- */
    char ztxt[48];
    snprintf(ztxt, sizeof(ztxt), "%s画面",
             ui_events_zone_name(config_get()->camera_zone));
    g_cam_text_lbl = uiw_label(cam, ztxt, CLR_TEXT_DIM);
    if (g_cam_text_lbl) {
        lv_obj_set_style_text_font(g_cam_text_lbl, &lv_font_SHSC_16, 0);
        /* 下移至两行头部下方的画面区中心 */
        lv_obj_align(g_cam_text_lbl, LV_ALIGN_CENTER, 0,
                     (CAM_PIC_Y + CAM_H) / 2 - CAM_H / 2 + 10);
        lv_label_set_long_mode(g_cam_text_lbl, LV_LABEL_LONG_DOT);
    }

    /* ---- 抓拍按钮（右下角） ---- */
    {
        lv_obj_t *cap = lv_button_create(cam);
        if (cap) {
            lv_obj_set_size(cap, 80, 28);
            lv_obj_align(cap, LV_ALIGN_BOTTOM_RIGHT, -10, -10);
            lv_obj_set_style_bg_opa(cap, LV_OPA_10, 0);
            lv_obj_set_style_bg_color(cap, lv_color_hex(CLR_AMBER), 0);
            lv_obj_set_style_border_color(cap, lv_color_hex(CLR_AMBER), 0);
            lv_obj_set_style_border_width(cap, 1, 0);
            lv_obj_set_style_radius(cap, 0, 0);
            lv_obj_set_style_pad_all(cap, 0, 0);
            lv_obj_clear_flag(cap, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_t *cl = uiw_label(cap, "抓拍",
                                      CLR_AMBER);
            if (cl) {
                lv_obj_center(cl);
                lv_label_set_long_mode(cl, LV_LABEL_LONG_DOT);
            }
            lv_obj_add_event_cb(cap, _on_capture, LV_EVENT_CLICKED, NULL);
        }
    }
}

static void _build_zones_grid(lv_obj_t *par)
{
    lv_obj_t *grid = uiw_obj(par);
    if (!grid) return;
    /* 固定高度 = 左列472 - 396(cam) - 6(gap) = 70 */
    lv_obj_set_size(grid, LEFT_W, (MAIN_H - 12) - CAM_H - GAP_SM);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_gap(grid, GAP_SM, 0);
    lv_obj_clear_flag(grid, LV_OBJ_FLAG_SCROLLABLE);
    g_zones_grid = grid;
    _build_zone_card();
}

void _build_zone_card(void)
{
    if (!g_zones_grid) return;
    lv_obj_clean(g_zones_grid);
    g_zone_cool_lbl = NULL;

    for (int i = 0; i < ZONE_COUNT; i++) {
        g_zone_cards[i] = NULL;
        g_zone_status[i] = NULL;
        g_zone_dots[i] = NULL;
    }

    int cz = config_get()->camera_zone;
    if (cz < 0 || cz >= ZONE_COUNT) cz = 0;
    g_zone_card_idx = cz;

    lv_obj_t *card = uiw_obj(g_zones_grid);
    if (!card) return;
    lv_obj_set_size(card, LEFT_W, 70);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(CLR_CARD), 0);
    lv_obj_set_style_border_color(card, lv_color_hex(CLR_BORDER), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    g_zone_cards[cz] = card;

    g_zone_dots[cz] = uiw_dot(card, CLR_GREEN, 8);
    if (g_zone_dots[cz]) lv_obj_align(g_zone_dots[cz], LV_ALIGN_TOP_LEFT, 14, 18);

    lv_obj_t *name_lbl = uiw_label_font(card, zone_info[cz].name,
                                        CLR_TEXT_HI, &lv_font_SHSC_16);
    if (name_lbl) {
        lv_label_set_long_mode(name_lbl, LV_LABEL_LONG_DOT);
        lv_obj_align(name_lbl, LV_ALIGN_TOP_LEFT, 30, 14);
    }

    /* 右侧提示：区域由云端配置 */
    /* 右侧提示：冷却剩余时间 / 区域来源 */
    g_zone_cool_lbl = uiw_label_cn(card, "区域由云端配置", CLR_TEXT_DIM);
    if (g_zone_cool_lbl) lv_obj_align(g_zone_cool_lbl, LV_ALIGN_TOP_RIGHT, -12, 16);

    g_zone_status[cz] = uiw_label(card, "已撤防", CLR_TEXT_LO);
    if (g_zone_status[cz]) {
        lv_label_set_long_mode(g_zone_status[cz], LV_LABEL_LONG_DOT);
        lv_obj_align(g_zone_status[cz], LV_ALIGN_TOP_LEFT, 14, 40);
    }

    lv_obj_add_event_cb(card, ui_detail_card_click_cb, LV_EVENT_CLICKED,
                       (void *)(intptr_t)cz);
    _update_zone_visual(cz);
}

static void _build_right_col(lv_obj_t *par)
{
    lv_obj_t *col = uiw_obj(par);
    if (!col) return;
    lv_obj_set_size(col, RIGHT_W, MAIN_H);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(col, 0, 0);
    lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE);

    /* 事件时间轴（header 32 + 列表 316）：构建见 ui/panels/ui_timeline */
    ui_timeline_build(col, RIGHT_W, 348);

    /* ---- 传感器迷你面板（主界面快速观察） ---- */
    lv_obj_t *mini = uiw_obj(col);
    if (mini) {
        lv_obj_set_size(mini, RIGHT_W, MAIN_H - 348);
        lv_obj_set_flex_flow(mini, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_all(mini, 5, 0);
        lv_obj_set_style_pad_gap(mini, 2, 0);
        lv_obj_set_style_bg_color(mini, lv_color_hex(CLR_CARD), 0);
        lv_obj_set_style_bg_opa(mini, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(mini, lv_color_hex(CLR_BORDER), 0);
        lv_obj_set_style_border_width(mini, 1, 0);
        lv_obj_set_style_radius(mini, 0, 0);
        lv_obj_set_style_outline_width(mini, 0, 0);
        lv_obj_set_style_shadow_width(mini, 0, 0);
        lv_obj_clear_flag(mini, LV_OBJ_FLAG_SCROLLABLE);

        uiw_label_font(mini, "传感器", CLR_TEXT_LO, FONT_UI);

        static const char *mk[3] = { "AP3216C", "ICM20608", "摄像头" };
        for (int i = 0; i < 3; i++) {
            lv_obj_t *row = uiw_obj(mini);
            if (!row) continue;
            lv_obj_set_size(row, RIGHT_W - 16, 26);
            lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                                  LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
            uiw_label_font(row, mk[i], CLR_TEXT_DIM, FONT_UI);
            g_mini_lbl[i] = uiw_label_font(row, "--", CLR_CYAN, FONT_UI);
        }

        g_mini_chart = lv_chart_create(mini);
        if (g_mini_chart) {
            lv_obj_set_size(g_mini_chart, RIGHT_W - 16, 50);
            lv_chart_set_type(g_mini_chart, LV_CHART_TYPE_LINE);
            lv_chart_set_point_count(g_mini_chart, 20);
            lv_chart_set_range(g_mini_chart, LV_CHART_AXIS_PRIMARY_Y, 0, 600);
            lv_chart_set_div_line_count(g_mini_chart, 2, 4);
            lv_obj_set_style_bg_opa(g_mini_chart, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(g_mini_chart, 0, 0);
            lv_obj_set_style_outline_width(g_mini_chart, 0, 0);
            lv_obj_set_style_pad_all(g_mini_chart, 2, 0);
            lv_obj_set_style_line_color(g_mini_chart, lv_color_hex(CLR_BORDER), LV_PART_MAIN);
            lv_obj_set_style_line_opa(g_mini_chart, LV_OPA_40, LV_PART_MAIN);
            lv_obj_set_style_line_width(g_mini_chart, 1, LV_PART_MAIN);
            lv_obj_set_style_line_width(g_mini_chart, 1, LV_PART_ITEMS);
            lv_obj_set_style_width(g_mini_chart, 3, LV_PART_INDICATOR);
            lv_obj_set_style_height(g_mini_chart, 3, LV_PART_INDICATOR);
            g_mini_ser = lv_chart_add_series(g_mini_chart, lv_color_hex(CLR_CYAN),
                                            LV_CHART_AXIS_PRIMARY_Y);
        }
    }
}

/* 设置按钮：打开设置弹窗（实现见 ui/modals/ui_settings.c） */
static void _ui_settings_btn_cb(lv_event_t *e)
{
    (void)e;
    ui_settings_open();
}

static void _build_bottom_bar(lv_obj_t *p)
{
    lv_obj_t *bar = uiw_obj(p);    if (!bar) return;
    lv_obj_set_size(bar, SCR_W, BOTTOM_H);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(CLR_CARD), 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_color(bar, lv_color_hex(CLR_BORDER), 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(bar, 12, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    /* 布防 (青色) */
    lv_obj_t *b1 = uiw_op_btn(bar, "布防",
                               CLR_CYAN, CLR_CYAN, CLR_CYAN, LV_OPA_20, 130);
    if (b1) {
        lv_obj_clear_flag(b1, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(b1, _on_arm, LV_EVENT_CLICKED, NULL);
    }

    /* 撤防 (灰色) */
    lv_obj_t *b2 = uiw_op_btn(bar, "撤防",
                               CLR_BORDER_LT, CLR_BORDER_LT, CLR_TEXT_LO, LV_OPA_10, 130);
    if (b2) {
        lv_obj_clear_flag(b2, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(b2, _on_disarm, LV_EVENT_CLICKED, NULL);
    }

    /* 消警 (琥珀) */
    lv_obj_t *b3 = uiw_op_btn(bar, "消警",
                               CLR_AMBER, CLR_AMBER, CLR_AMBER, LV_OPA_10, 130);
    if (b3) {
        lv_obj_clear_flag(b3, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(b3, _on_silence, LV_EVENT_CLICKED, NULL);
    }

    /* 设置 (深色) */
    lv_obj_t *b4 = uiw_op_btn(bar, "设置",
                               CLR_BORDER, CLR_BORDER, CLR_TEXT_LO, LV_OPA_30, 130);
    if (b4) {
        lv_obj_clear_flag(b4, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(b4, _ui_settings_btn_cb, LV_EVENT_CLICKED, NULL);
    }

    /* 测试 (青绿，模块状态诊断) */
    lv_obj_t *b5 = uiw_op_btn(bar, "检测",
                               CLR_GREEN, CLR_GREEN, CLR_GREEN, LV_OPA_10, 130);
    if (b5) {
        lv_obj_clear_flag(b5, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(b5, _on_test, LV_EVENT_CLICKED, NULL);
    }
}

/* 事件时间轴写入：UI 列表（ui_timeline）+ 云端上报（MQTT fire-and-forget） */
void _add_event(const char *title, const char *loc,
                uint32_t dot_clr, const char *level)
{
    if (!title || !ui_timeline_ready()) return;

    ui_timeline_add(title, loc, dot_clr, level);

    char t[8];
    ui_time_str(t, sizeof(t));
    mqtt_hub_publish_event(title, loc ? loc : "", level ? level : "", t);
}

/* 联网状态检测（socket 细节在 infra/net_probe） */
static int net_check(void)
{
    return net_probe_tcp("192.168.3.1", 80, 200);   /* 板端网关 IP */
}

static void net_timer_cb(lv_timer_t *t)
{
    (void)t;
    ui_events_net_set(net_check());
}

/* 外设桥接轮询（原 dev_bridge 定时器，本层只调 poll） */
static void dev_poll_cb(lv_timer_t *t)
{
    (void)t;
    dev_bridge_poll();
}

/* 领域事件 → UI（dev_bridge 等只发事件，不直接调 UI） */
static void ui_events_dispatch_cb(lv_timer_t *t)
{
    (void)t;
    system_event_t ev;
    while (event_bus_poll(&ev)) {
        if (ev.type == EV_ALARM) {
            if (ev.code == 1)
                ui_events_alarm_trigger_src(ev.source, ev.text);
            else if (ev.code == 0)
                ui_events_alarm_auto_clear(ev.source, ev.text);
        } else if (ev.type == EV_SYS) {
            if (ev.code == 1 || ev.code == 5)
                ui_events_config_changed();
            else if (ev.code == 2)
                ui_events_log(ev.text, ev.loc, ev.level);
            else if (ev.code == 3)
                ui_events_toast(ev.text);
            else if (ev.code == 4)
                ui_events_user_activity();
        }
    }
}
/* ================================================================
 * 配置持久化：UI 改动只更新内存，1s 定时器统一落盘
 * ================================================================ */
void _cfg_save_ui(void)
{
    struct app_config c = *config_get();
    c.cloud_review = (g_cloud_review == CLOUD_REVIEW_ON) ? 1 : 0;
    c.motion_interval = cam_feed_get_motion_interval();
    c.motion_diff = cam_feed_get_motion_diff();
    c.brightness = g_brightness;
    c.schedule_on = g_schedule_on;
    c.sched_start_h = g_sched_start_h;
    c.sched_end_h = g_sched_end_h;
    config_update(&c);
}

static void _config_flush_cb(lv_timer_t *t)
{
    (void)t;
    config_flush();
}

/* ================================================================
 * 设置弹窗回调：亮度滑条 + 定时布防开关
 * ================================================================ */

/* ---- 云端复核开关（控件在 ui_settings，这里同步按钮 + 摄像头徽章） ---- */
void _review_refresh(void)
{
    int on = (g_cloud_review == CLOUD_REVIEW_ON);
    ui_settings_refresh_review(on);

    /* 摄像头区 mode-badge 联动 */
    if (g_mode_badge_lbl)
        lv_label_set_text(g_mode_badge_lbl,
                          on ? "云端复核模式" : "纯本地模式");
}

/* ---- 检测设置（运动粗判参数：帧差间隔 + 灰度差阈值，08-29） ---- */

void _motion_param_refresh(void)
{
    ui_settings_refresh_motion();
}

/* 步进按钮（−/+）助手 */

/* ---- 预警设置（四位置 × 触发源 × 数据通道 × 阈值，写入 dev_bridge 实时生效） ---- */

/* 位置名（字库内字） */
static const char *g_zone_names[4] = { "前门", "后门", "窗户", "仓库" };

/* 触发源/通道按钮组高亮刷新 */

/* 选择触发源 */

/* 选择数据通道 */

/* 选择比较方式（GT=大于 EQ=等于 LT=小于） */

/* 阈值滑条 */

/* C2 双阈值：预警阈值滑条（0=关闭，达到只轻提醒） */

/* ---- 消警冷却时间（默认 60s） ---- */

/* 根据当前位置配置刷新按钮高亮/通道可见性/阈值 */

/* ---- LCD 背光控制（pwm-backlight sysfs） ---- */
static int g_backlight_max = 100;

static int _backlight_init(void)
{
    /* 读 max_brightness */
    int fd = open("/sys/class/backlight/backlight/max_brightness", O_RDONLY);
    if (fd >= 0) {
        char buf[16] = {0};
        if (read(fd, buf, sizeof(buf) - 1) > 0)
            g_backlight_max = atoi(buf);
        close(fd);
    }
    return g_backlight_max > 0 ? g_backlight_max : 100;
}

/* 亮度百分比 → 背光 sysfs（0-100%） */
void _backlight_set(int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    int fd = open("/sys/class/backlight/backlight/brightness", O_WRONLY);
    if (fd < 0) return;
    if (lseek(fd, 0, SEEK_SET) >= 0) {
        char buf[16];
        int val = percent * _backlight_init() / 100;
        snprintf(buf, sizeof(buf), "%d", val);
        if (write(fd, buf, strlen(buf)) < 0) LOGE("UI", "backlight write: %s", strerror(errno));
    }
    close(fd);
}

/* 读取当前背光百分比（初始化滑条用） */
int _backlight_get(void)
{
    int fd = open("/sys/class/backlight/backlight/brightness", O_RDONLY);
    if (fd < 0) return 80;
    char buf[16] = {0};
    if (read(fd, buf, sizeof(buf) - 1) <= 0) { close(fd); return 80; }
    close(fd);
    int v = atoi(buf);
    int max = _backlight_init();
    return (max > 0) ? (v * 100 / max) : 80;
}

/* ---- 定时布防时间段设置 ---- */

/* ================================================================
 * 设置弹窗
 * ================================================================ */

/* ---- 重启应用（摄像头卡死时的恢复手段） ---- */

/* ================================================================
 * 回调：事件"查看"按钮 — 打开相册弹窗（由 ui_timeline 回调）
 * ================================================================ */

/* ================================================================
 * 相册弹窗（HTML #album-modal）：3×2 真实抓拍缩略图
 * 数据来自 g_snaps（抓拍按钮/预警触发经 snapshot_capture 写入）
 * TF 持久化/容量/恢复由 storage_mgr 组件提供
 * ================================================================ */

/* 抓拍：拷当前摄像头帧 → 全图 + 缩略图降采样 + 元数据 → TF 持久化 → 相册重绘
 * zone_idx 用于 TF 文件名编码（重启恢复时还原区域名） */

/* 级别 → 颜色/文字（与事件徽章一致） */

/* 缩略图点击 → 打开查看弹窗 */

/* 重绘缩略图网格（数据变化后调用）：真实缩略图 + 左上角区域/级别 + 右下角时间 */

/* ================================================================
 * 测试模块弹窗：显示各外设状态与实时数据（1s 刷新）
 * 数据来源 dev_bridge_get_diag()
 * ================================================================ */

/* ---- 测试面板控制回调 ---- */

/* ================================================================
 * 启动恢复：扫描 TF 上 snap_*.bmp（mtime 最新 6 张）回填内存相册
 * 文件名协议: snap_z<zone>_<cmp>_<HHMMSS>.bmp → 还原区域/级别/时间
 * ================================================================ */

/* ================================================================
 * 查看弹窗：显示缩略图对应的全尺寸照片（630×340，与主页显示区一致）
 * 关闭：标题栏 X / 点击遮罩（uiw_modal_create 内置）
 * ================================================================ */

/* ================================================================
 * ui_home_create — 主入口
 * ================================================================ */
/* ---- 告警弹窗 hooks：把 UI 刷新/记录/静音能力注入 ui_alarm 模块 ---- */
static void _alarm_on_zone_ack(int zone)
{
    _update_zone_visual(zone);
}

static void _alarm_on_confirmed(void)
{
    _add_event("告警确认", "系统", CLR_GREEN, "");
    _show_toast("告警已确认");
}

static void _alarm_on_muted(void)
{
    dev_bridge_set_beep_mute(1);
    _add_event("告警静音", "系统", CLR_AMBER, "");
}

static void _wire_alarm_hooks(void)
{
    ui_alarm_hooks_t h;
    h.on_zone_ack = _alarm_on_zone_ack;
    h.on_confirmed = _alarm_on_confirmed;
    h.on_muted = _alarm_on_muted;
    ui_alarm_set_hooks(&h);
}

/* 主页右侧迷你传感器面板：文本 1s 刷新 */
static void _mini_panel_refresh(void)
{
    struct dev_diag d;
    dev_bridge_get_diag(&d);

    if (g_mini_lbl[0]) {
        char b[48];
        if (!d.als_en)
            snprintf(b, sizeof(b), "采样停");
        else
            snprintf(b, sizeof(b), "光照 %d | 接近 %d", d.als_als, d.als_ps);
        lv_label_set_text(g_mini_lbl[0], b);
    }
    if (g_mini_lbl[1]) {
        char b[48];
        if (!d.imu_en)
            snprintf(b, sizeof(b), "采样停");
        else
            snprintf(b, sizeof(b), "合 %d | 线 %d", d.imu_mag_x100, d.imu_lin_x100);
        lv_label_set_text(g_mini_lbl[1], b);
    }
    if (g_mini_lbl[2]) {
        char b[32];
        snprintf(b, sizeof(b), "%d fps | %s", cam_feed_get_fps(),
                 cam_feed_get_motion_en() ? "ON" : "OFF");
        lv_label_set_text(g_mini_lbl[2], b);
    }
}

/* 10Hz：主页迷你曲线 + 检测页采样/曲线 */
static void sens_timer_cb(lv_timer_t *t)
{
    (void)t;
    static int tick = 0;
    struct dev_diag d;
    dev_bridge_get_diag(&d);

    if (g_mini_chart && g_mini_ser && d.imu_en && (tick % 10) == 0)
        lv_chart_set_next_value(g_mini_chart, g_mini_ser, d.imu_lin_x100);

    if ((tick % 10) == 0)
        _mini_panel_refresh();

    ui_sensor_tick_10hz();
    tick++;
}

void ui_home_create(lv_obj_t *parent)
{
    if (!parent) return;

    /* 时间轴“查看”按钮 → 相册弹窗（回调注入，面板之间不互相 include） */
    ui_timeline_set_view_cb(ui_album_open);

    /* 加载持久化配置（main/sim 入口可能未初始化）：先恢复全局 UI 状态，
     * 再构建控件，保证设置弹窗/滑条/开关显示的是上次保存的值。 */
    if (!config_path()[0])
        config_init(NULL);
    {
        const struct app_config *cfg = config_get();
        g_cloud_review = cfg->cloud_review ? CLOUD_REVIEW_ON : CLOUD_REVIEW_OFF;
        g_brightness = cfg->brightness;
        g_schedule_on = cfg->schedule_on;
        g_sched_start_h = cfg->sched_start_h;
        g_sched_end_h = cfg->sched_end_h;
        cam_feed_set_motion_interval(cfg->motion_interval);
        cam_feed_set_motion_diff(cfg->motion_diff);
    }

    /* 根容器：flex-col 不可滚动 */
    lv_obj_set_size(parent, SCR_W, SCR_H);
    lv_obj_set_style_bg_color(parent, lv_color_hex(CLR_BG), 0);
    lv_obj_set_style_pad_all(parent, 0, 0);
    lv_obj_set_style_border_width(parent, 0, 0);
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollbar_mode(parent, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

    /* 初始化分区状态 */
    zone_state_init();

    /* 1. status-bar 36px */
    ui_status_bar_build(parent);

    /* 2. main-area  固定 1024×512（不使用 flex_grow，避免高度塌陷） */
    /*    512 = 600 - 36(status) - 52(bottom)，info-bar 已删除，空间补偿给主区 */
    lv_obj_t *ma = uiw_obj(parent);
    if (ma) {
        lv_obj_set_size(ma, SCR_W, MAIN_H);
        lv_obj_set_flex_flow(ma, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(ma, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
        lv_obj_set_style_pad_left(ma, 8, 0);
        lv_obj_set_style_pad_right(ma, 8, 0);
        lv_obj_set_style_pad_top(ma, GAP_SM, 0);
        lv_obj_set_style_pad_bottom(ma, GAP_SM, 0);
        lv_obj_set_style_pad_gap(ma, GAP_MD, 0);
        lv_obj_clear_flag(ma, LV_OBJ_FLAG_SCROLLABLE);

        /* 左列 630×500（固定高度 = MAIN_H - 上下padding 12，不依赖 flex_grow） */
        lv_obj_t *left = uiw_obj(ma);
        if (left) {
            lv_obj_set_size(left, LEFT_W, MAIN_H - 12);
            lv_obj_set_flex_flow(left, LV_FLEX_FLOW_COLUMN);
            lv_obj_set_flex_align(left, LV_FLEX_ALIGN_START,
                                  LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
            lv_obj_set_style_pad_gap(left, GAP_SM, 0);
            lv_obj_clear_flag(left, LV_OBJ_FLAG_SCROLLABLE);

            _build_cam_wrap(left);
            _build_zones_grid(left);
        }

        /* 右列 360×512 */
        _build_right_col(ma);
    }

    /* 3. bottom-bar 52px */
    _build_bottom_bar(parent);

    /* 4. Toast 提示（top layer 独立顶层，底部居中显示） */
    g_toast = uiw_label(lv_layer_top(), "", CLR_BLACK);
    if (g_toast) {
        lv_label_set_long_mode(g_toast, LV_LABEL_LONG_DOT);
        lv_obj_set_style_bg_opa(g_toast, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(g_toast, lv_color_hex(CLR_GREEN), 0);
        lv_obj_set_style_pad_left(g_toast, 18, 0);
        lv_obj_set_style_pad_right(g_toast, 18, 0);
        lv_obj_set_style_pad_top(g_toast, 6, 0);
        lv_obj_set_style_pad_bottom(g_toast, 6, 0);
        lv_obj_align(g_toast, LV_ALIGN_CENTER, 0, 0);
        lv_obj_add_flag(g_toast, LV_OBJ_FLAG_HIDDEN);
    }

    /* 5. 弹窗（初始隐藏） */
    ui_detail_build();
    ui_settings_build();
    ui_album_build();
    ui_sensor_build();
    _wire_alarm_hooks();
    ui_alarm_build();

    /* 6. 抓拍恢复：从 TF 回填上次开机的抓拍相册 */
    ui_album_restore();

    /* 触摸唤醒：任意触摸重置空闲计时并解除 blank */
    lv_obj_add_event_cb(parent, _on_screen_touch, LV_EVENT_PRESSED, NULL);

    /* 启动自愈：强制解除 fb blank（08-30 板测定位——App 前板子停在登录提示符
     * 超过内核空闲阈值时，fbcon 会 blank 掉 fb；App 启动若不 unblank，
     * 渲染/刷写照常但 LCDIF 已停扫描 → 黑屏且背光亮、vsync 等待超时） */
    _screen_set_blank(0);

    /* 应用持久化亮度（重启后恢复上次设置；模拟器上 sysfs 不存在时静默失败） */
    _backlight_set(g_brightness);

    /* 6. 创建定时器 */
    g_timer_clock = lv_timer_create(_timer_clock_cb, 1000, NULL);
    g_timer_anim  = lv_timer_create(_timer_anim_cb, 50, NULL);
    lv_timer_create(net_timer_cb, 5000, NULL);      /* 联网检测 5s */
    lv_timer_create(_config_flush_cb, 1000, NULL);  /* 配置 dirty 落盘 1s */
    lv_timer_create(sens_timer_cb, 100, NULL);      /* 传感器采样/曲线 10Hz */
    lv_timer_create(dev_poll_cb, 200, NULL);        /* 外设桥接轮询（原 dev_bridge 定时器） */
    lv_timer_create(ui_events_dispatch_cb, 50, NULL); /* 领域事件 → UI */

    /* 7. 摄像头喂数（V4L2 线程 + 33ms 帧刷新定时器，30fps） */
    if (g_cam_canvas) {
        cam_feed_start(g_cam_canvas_buf);
        lv_timer_create(_cam_refresh_cb, 33, NULL);
    }
}

/* 入侵框描画（RGB565 画布缓冲，2px 红框；坐标=画布 630×340 空间） */

/* 摄像头帧刷新：拷贝最新完成帧（双缓冲发布）+ 叠加入侵框后局部 invalidate
 * （v9 canvas 是普通对象，用 lv_obj_invalidate）。
 * 附：有画面隐藏占位文字/网格线，断流自动恢复（需求 08-29） */
static void _cam_refresh_cb(lv_timer_t *timer)
{
    (void)timer;
    if (g_screen_blanked) return;   /* 息屏期间不拷贝/不重绘，唤醒后整帧刷新 */
    /* 撕裂保护：显示尚未锁相时优先丢帧，但绝不能“永久丢”：
     * 唤醒后 g_cam_recover_frames 窗口内全速放行；窗口结束后若 VSYNC 仍未恢复，
     * 每 10 帧（约 330ms）放行 1 帧，促使 flush 重新锁相，避免画面彻底卡死。 */
    if (g_cam_recover_frames > 0) {
        g_cam_recover_frames--;
    } else if (!lvgl_port_vsync_ok()) {
        static int skip = 0;
        if (++skip % 10 != 0) return;
    }

    if (g_cam_canvas && cam_feed_blit_if_ready(g_cam_canvas_buf)) {
        /* 入侵框叠加（精判检出框，3s 保持窗；随帧重描，下一帧 blit 前重画） */
        detector_box_t boxes[8];
        int nb = detector_get_boxes(boxes, 8);
        for (int i = 0; i < nb; i++)
            ui_canvas_draw_box(g_cam_canvas_buf, CAM_DISP_W, CAM_DISP_H,
                                       boxes[i].x, boxes[i].y,
                                       boxes[i].w, boxes[i].h, 0xFB29);
        lv_obj_invalidate(g_cam_canvas);
    }

    if (g_cam_grid_cnt > 0 || g_cam_text_lbl) {
        int hide = cam_feed_stream_ok();
        if (hide != g_cam_overlay_hidden) {
            for (int i = 0; i < g_cam_grid_cnt; i++) {
                if (!g_cam_grid_lines[i]) continue;
                if (hide) lv_obj_add_flag(g_cam_grid_lines[i], LV_OBJ_FLAG_HIDDEN);
                else      lv_obj_clear_flag(g_cam_grid_lines[i], LV_OBJ_FLAG_HIDDEN);
            }
            if (g_cam_text_lbl) {
                if (hide) lv_obj_add_flag(g_cam_text_lbl, LV_OBJ_FLAG_HIDDEN);
                else      lv_obj_clear_flag(g_cam_text_lbl, LV_OBJ_FLAG_HIDDEN);
            }
            g_cam_overlay_hidden = hide;
        }
    }
}

/* ui_events 实现已移到 ui/ui_events.c（本文件只负责界面构建） */
