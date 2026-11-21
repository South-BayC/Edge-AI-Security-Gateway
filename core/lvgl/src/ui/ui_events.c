/**
 * ui_events.c — 真实状态信号接入层实现（从 ui_home.c 抽出）
 *
 * 板端传感器/摄像头/网络/RTC 通过 ui_events.h 的接口驱动 UI：
 * 状态显示、事件时间轴、抓拍相册、告警弹窗。
 *
 * 依赖：ui_internal.h（主页共享对象/刷新能力）、ui_theme.h（色板）、
 *       domain/zone_state（状态真值）、config_mgr（配置读取）。
 * 不直接操作硬件或网络。
 */
#include "ui_events.h"
#include "ui_internal.h"
#include "ui_theme.h"
#include "panels/ui_status_bar.h"
#include "modals/ui_alarm.h"
#include "modals/ui_settings.h"
#include "modals/ui_album.h"
#include "../domain/zone_state.h"
#include "../config_mgr.h"
#include "../log_svc.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

/* 联网状态缓存（用于状态变化事件去重） */
static int g_net_online = 1;
static int g_net_online_init = 0;

void ui_events_zone_set_state(int id, zone_state_t st)
{
    if (id < 0 || id >= ZONE_COUNT) return;
    if (zone_state_get(id) == st) return;   /* 无变化不刷新 */

    zone_state_set(id, st);
    _update_zone_visual(id);
}

void ui_events_alarm_trigger_src(int id, const char *src)
{
    if (id < 0 || id >= ZONE_COUNT) return;
    if (zone_state_get(id) == ZONE_ALARM) return;

    zone_state_set(id, ZONE_ALARM);
    _update_zone_visual(id);

    /* 预警自动抓拍：截当前画面存入相册（区域=告警分区，级别=high） */
    ui_album_capture(id, zone_info[id].name, "high");

    /* 触发类型写入事件标题："SENSOR 触发" / "CAM 触发"（未来摄像头预警复用） */
    char title[32];
    snprintf(title, sizeof(title), "%s 触发", (src && src[0]) ? src : "SENSOR");
    _add_event(title, zone_info[id].name, CLR_RED, "high");
    _show_toast("异常 已触发");

    /* 告警确认弹窗（设计 P0：长按 1s 消警防误触 + MUTE 静音） */
    ui_alarm_open(id, zone_info[id].name, src);
}

void ui_events_alarm_trigger(int id)
{
    ui_events_alarm_trigger_src(id, "SENSOR");
}

void ui_events_alarm_ack(int id)
{
    if (id < 0 || id >= ZONE_COUNT) return;
    if (zone_state_get(id) != ZONE_ALARM) return;

    ui_alarm_clear_fsm(id);
    zone_state_ack(id);
    _update_zone_visual(id);
    _add_event("报警已消", zone_info[id].name, CLR_GREEN, "");
}

int ui_events_zone_is_alarm(int id)
{
    if (id < 0 || id >= ZONE_COUNT) return 0;
    return zone_state_get(id) == ZONE_ALARM;
}

void ui_events_alarm_auto_clear(int id, const char *reason)
{
    if (id < 0 || id >= ZONE_COUNT) return;
    if (zone_state_get(id) != ZONE_ALARM) return;   /* 不在告警则无动作（幂等） */

    zone_state_ack(id);
    _update_zone_visual(id);
    char t[64];
    snprintf(t, sizeof(t), "告警解除：%s", reason && reason[0] ? reason : "云端复核");
    _add_event(t, zone_info[id].name, CLR_GREEN, "");
    _show_toast("云端复核消警");
    /* 告警弹窗隐藏（等价人工长按消警的关窗动作；声光由 dev_bridge 轮询自动停） */
    ui_alarm_hide();
}

void ui_events_toast(const char *text)
{
    _show_toast(text ? text : "");
}

void ui_events_log(const char *title, const char *loc, const char *level)
{
    _add_event(title ? title : "", loc ? loc : "系统",
               (level && strcmp(level, "high") == 0) ? CLR_RED :
               (level && strcmp(level, "medium") == 0) ? CLR_AMBER : CLR_CYAN,
               level ? level : "");
}

void ui_events_net_set(int online)
{
    ui_status_bar_set_net(online);

    /* 状态变化时写事件（首次调用不写，避免启动噪音） */
    if (g_net_online_init && g_net_online != online)
        _add_event(online ? "网络在线" : "网络离线", "主控",
                   online ? CLR_GREEN : CLR_AMBER, "");
    g_net_online = online;
    g_net_online_init = 1;
}

void ui_events_set_time(int h, int m, int s)
{
    /* 直接设置系统时间（settimeofday），时钟定时器下一秒自动跟随显示。
     * 外部真实时间源（RTC 芯片/NTP 同步程序）调用此接口即可驱动 UI。 */
    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm tm_cur;
    time_t now = tv.tv_sec;
    localtime_r(&now, &tm_cur);
    tm_cur.tm_hour = h % 24;
    tm_cur.tm_min  = m % 60;
    tm_cur.tm_sec  = s % 60;
    tv.tv_sec = mktime(&tm_cur);
    if (settimeofday(&tv, NULL) != 0)
        LOGE("UI", "settimeofday failed: %s", strerror(errno));
}

int ui_events_is_armed(void)
{
    /* 任一分区布防即视为已布防（支持分区级独立布防/撤防） */
    return zone_state_any_armed();
}

int ui_events_zone_has_armed(void)
{
    return zone_state_any_armed();
}

int ui_events_zone_is_armed(int id)
{
    return zone_state_is_armed(id);
}

int ui_events_current_cam_zone(void)
{
    /* 单摄固定安装：摄像头归属由配置 camera_zone 决定（0~3） */
    int z = config_get()->camera_zone;
    if (z < 0 || z >= ZONE_COUNT) z = 0;
    return z;
}

const char *ui_events_zone_name(int id)
{
    return zone_state_name(id);
}

int ui_events_zone_has_alarm(void)
{
    return zone_state_any_alarm();
}

int ui_events_zone_alarm_level(void)
{
    return zone_state_alarm_level();
}

/* T10：远程配置变更后刷新 UI（亮度/云端复核/检测参数/定时布防） */
void ui_events_config_changed(void)
{
    const struct app_config *c = config_get();

    g_cloud_review = c->cloud_review ? CLOUD_REVIEW_ON : CLOUD_REVIEW_OFF;
    g_brightness = c->brightness;
    g_schedule_on = c->schedule_on;
    g_sched_start_h = c->sched_start_h;
    g_sched_end_h = c->sched_end_h;

    if (c->camera_zone != g_zone_card_idx)
        _build_zone_card();   /* 云端改区域后，主界面卡片跟随 */

    _review_refresh();
    _motion_param_refresh();
    _backlight_set(g_brightness);
    ui_settings_sync_from_config();
}

/* 云端复核开关（设置-云端复核；dev_bridge/detector 据此定案：两级管线或纯本地） */
int ui_events_cloud_review_on(void)
{
    return g_cloud_review == CLOUD_REVIEW_ON;
}

void ui_events_user_activity(void)
{
    g_idle_sec = 0;             /* 重置空闲计时（重新 10 分钟计时） */
    _screen_set_blank(0);       /* 解除 blank 唤醒屏幕（fb 内容仍在，直接恢复） */

    /* 全屏 invalidate：下一帧（~5ms）自动重绘，消除 blank 恢复的撕裂。
     * 注意：不能在事件回调里用 lv_refr_now() 同步刷新——会阻塞 LVGL
     * 主线程导致触摸事件丢失（表现为界面卡死无响应）。 */
    lv_obj_invalidate(lv_screen_active());
}
