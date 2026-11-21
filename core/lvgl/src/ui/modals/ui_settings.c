/**
 * ui_settings.c — 设置弹窗实现（从 ui_home.c 抽出）
 *
 * 控件与回调由本模块持有；背光 sysfs 写入、配置落盘、摄像头徽章刷新
 * 仍由 ui_home 提供（ui_internal.h 的 ui 层内部接口）。
 */
#include "ui_settings.h"
#include "../ui_widgets.h"
#include "../ui_theme.h"
#include "../ui_internal.h"
#include "../../config_mgr.h"
#include "../../dev_bridge.h"
#include "../../cam_feed.h"
#include "../../health_mgr.h"

#include <stdio.h>
#include <string.h>

/* ---- 设置页控件 ---- */
static lv_obj_t *s_overlay = NULL;
static lv_obj_t *g_bright_lbl = NULL;
static lv_obj_t *g_dm_btn = NULL;
static lv_obj_t *g_sched_toggle = NULL;
static lv_obj_t *g_sched_knob = NULL;
static lv_obj_t *g_sched_hint = NULL;
static lv_obj_t *g_sched_start_lbl = NULL;
static lv_obj_t *g_sched_end_lbl = NULL;

static lv_obj_t *g_itv_lbl = NULL;
static lv_obj_t *g_diff_lbl = NULL;

static lv_obj_t *g_warn_src_btns[3] = {NULL, NULL, NULL};
static lv_obj_t *g_warn_ch_btns[5] = {NULL, NULL, NULL, NULL, NULL};
static lv_obj_t *g_warn_cmp_btns[3] = {NULL, NULL, NULL};
static lv_obj_t *g_warn_thr_lbl = NULL;
static lv_obj_t *g_warn_thr_sl = NULL;
static lv_obj_t *g_warn_pre_lbl = NULL;
static lv_obj_t *g_warn_pre_sl = NULL;
static lv_obj_t *g_cool_lbl = NULL;
static int g_warn_zone_sel = 0;            /* 当前编辑的预警分区 0-3 */

/* 前向声明（本文件内互调） */
static void _warn_refresh_ui(void);
static void _on_restart_app(lv_event_t *e);

/* 小步进按钮（- / +） */
static lv_obj_t *_step_btn(lv_obj_t *par, const char *txt, lv_event_cb_t cb)
{
    lv_obj_t *b = lv_button_create(par);
    if (!b) return NULL;
    lv_obj_set_size(b, 44, 30);
    lv_obj_set_style_radius(b, 0, 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(CLR_BORDER), 0);
    lv_obj_t *l = uiw_label(b, txt, CLR_CYAN);
    if (l) lv_obj_center(l);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    return b;
}


static void _on_brightness_cb(lv_event_t *e)
{
    lv_obj_t *sl = lv_event_get_target(e);
    g_brightness = (int)lv_slider_get_value(sl);
    if (g_bright_lbl) {
        char b[8];
        snprintf(b, sizeof(b), "%d%%", g_brightness);
        lv_label_set_text(g_bright_lbl, b);
    }
    _backlight_set(g_brightness);   /* 画面亮度 ↔ LCD 背光 */
    _cfg_save_ui();
}

static void _on_review_toggle(lv_event_t *e)
{
    (void)e;
    g_cloud_review = (g_cloud_review == CLOUD_REVIEW_ON) ? CLOUD_REVIEW_OFF : CLOUD_REVIEW_ON;
    _review_refresh();
    _cfg_save_ui();
}

static void _on_itv_plus(lv_event_t *e)
{
    (void)e;
    cam_feed_set_motion_interval(cam_feed_get_motion_interval() + 1);
    _motion_param_refresh();
    _cfg_save_ui();
}

static void _on_itv_minus(lv_event_t *e)
{
    (void)e;
    cam_feed_set_motion_interval(cam_feed_get_motion_interval() - 1);
    _motion_param_refresh();
    _cfg_save_ui();
}

static void _on_diff_plus(lv_event_t *e)
{
    (void)e;
    cam_feed_set_motion_diff(cam_feed_get_motion_diff() + 5);
    _motion_param_refresh();
    _cfg_save_ui();
}

static void _on_diff_minus(lv_event_t *e)
{
    (void)e;
    cam_feed_set_motion_diff(cam_feed_get_motion_diff() - 5);
    _motion_param_refresh();
    _cfg_save_ui();
}

static void _on_warn_src_sel(lv_event_t *e)
{
    struct dev_config cfg;
    dev_bridge_get_config(&cfg);
    int src = (int)(intptr_t)lv_event_get_user_data(e);
    cfg.zones[g_warn_zone_sel].src = src;
    cfg.zones[g_warn_zone_sel].enabled = (src != WARN_SRC_NONE);
    if (src == WARN_SRC_NONE) {
        /* 关闭时清掉旧通道 */
        cfg.zones[g_warn_zone_sel].channel = 0;
    } else if (cfg.zones[g_warn_zone_sel].channel >= 4) {
        cfg.zones[g_warn_zone_sel].channel = 0;
    }
    dev_bridge_set_config(&cfg);
    _warn_refresh_ui();
}

static void _on_warn_ch_sel(lv_event_t *e)
{
    struct dev_config cfg;
    dev_bridge_get_config(&cfg);
    int ch = (int)(intptr_t)lv_event_get_user_data(e);
    cfg.zones[g_warn_zone_sel].channel = ch;
    dev_bridge_set_config(&cfg);
    _warn_refresh_ui();
}

static void _on_warn_cmp_sel(lv_event_t *e)
{
    struct dev_config cfg;
    dev_bridge_get_config(&cfg);
    int cmp = (int)(intptr_t)lv_event_get_user_data(e);
    cfg.zones[g_warn_zone_sel].cmp = cmp;
    dev_bridge_set_config(&cfg);
    _warn_refresh_ui();
}

static void _on_warn_thr_cb(lv_event_t *e)
{
    lv_obj_t *sl = lv_event_get_target(e);
    struct dev_config cfg;
    dev_bridge_get_config(&cfg);
    cfg.zones[g_warn_zone_sel].threshold = (int)lv_slider_get_value(sl);
    dev_bridge_set_config(&cfg);
    if (g_warn_thr_lbl) {
        char b[8];
        snprintf(b, sizeof(b), "%d", cfg.zones[g_warn_zone_sel].threshold);
        lv_label_set_text(g_warn_thr_lbl, b);
    }
}

static void _on_warn_pre_cb(lv_event_t *e)
{
    lv_obj_t *sl = lv_event_get_target(e);
    struct dev_config cfg;
    dev_bridge_get_config(&cfg);
    cfg.zones[g_warn_zone_sel].threshold_warn = (int)lv_slider_get_value(sl);
    dev_bridge_set_config(&cfg);
    if (g_warn_pre_lbl) {
        char b[8];
        int v = cfg.zones[g_warn_zone_sel].threshold_warn;
        if (v > 0) snprintf(b, sizeof(b), "%d", v);
        else       snprintf(b, sizeof(b), "OFF");
        lv_label_set_text(g_warn_pre_lbl, b);
    }
}

static void _on_cool_cb(lv_event_t *e)
{
    lv_obj_t *sl = lv_event_get_target(e);
    struct dev_config cfg;
    dev_bridge_get_config(&cfg);
    cfg.cooldown_sec = (int)lv_slider_get_value(sl);
    dev_bridge_set_config(&cfg);
    if (g_cool_lbl) {
        char b[8];
        snprintf(b, sizeof(b), "%ds", cfg.cooldown_sec);
        lv_label_set_text(g_cool_lbl, b);
    }
}

static void _warn_refresh_ui(void)
{
    /* 单区域：预警设置固定编辑摄像头归属防区 */
    g_warn_zone_sel = config_get()->camera_zone;
    if (g_warn_zone_sel < 0 || g_warn_zone_sel > 3) g_warn_zone_sel = 0;

    struct dev_config cfg;
    dev_bridge_get_config(&cfg);
    struct zone_warn_cfg *zc = &cfg.zones[g_warn_zone_sel];

    /* 触发源按钮高亮 */
    for (int i = 0; i < 3; i++) {
        if (!g_warn_src_btns[i]) continue;
        int active = (i == zc->src);
        lv_obj_set_style_bg_opa(g_warn_src_btns[i], active ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_bg_color(g_warn_src_btns[i], active ? lv_color_hex(CLR_CYAN) : lv_color_hex(CLR_CYAN), 0);
        lv_obj_t *ch = lv_obj_get_child(g_warn_src_btns[i], 0);
        if (ch) lv_obj_set_style_text_color(ch, active ? lv_color_hex(0xFFFFFF) : lv_color_hex(CLR_CYAN), 0);
    }

    /* 数据通道按钮：文字与显隐随触发源联动
     *  AP3216C → PS/ALS/IR（3 个）；ICM → MAG/AX/AY/AZ/TEMP（5 个）；OFF → 全隐藏 */
    static const char *ap_ch_names[5]  = { "PS", "ALS", "IR", "", "" };
    static const char *icm_ch_names[5] = { "LIN", "AX", "AY", "AZ", "TEMP" };
    const char *const *names = (zc->src == WARN_SRC_ICM) ? icm_ch_names :
                               (zc->src == WARN_SRC_AP3216C) ? ap_ch_names : NULL;
    for (int i = 0; i < 5; i++) {
        if (!g_warn_ch_btns[i]) continue;
        if (names && names[i][0]) {
            lv_obj_clear_flag(g_warn_ch_btns[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_t *l = lv_obj_get_child(g_warn_ch_btns[i], 0);
            if (l) lv_label_set_text(l, names[i]);
            int active = (i == zc->channel);
            lv_obj_set_style_bg_opa(g_warn_ch_btns[i], active ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
            if (l) lv_obj_set_style_text_color(l, active ? lv_color_hex(0xFFFFFF) : lv_color_hex(CLR_CYAN), 0);
        } else {
            lv_obj_add_flag(g_warn_ch_btns[i], LV_OBJ_FLAG_HIDDEN);
        }
    }

    /* 比较方式按钮（GT/EQ/LT）高亮 */
    for (int i = 0; i < 3; i++) {
        if (!g_warn_cmp_btns[i]) continue;
        int active = (i == zc->cmp);
        lv_obj_set_style_bg_opa(g_warn_cmp_btns[i], active ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_t *l = lv_obj_get_child(g_warn_cmp_btns[i], 0);
        if (l) lv_obj_set_style_text_color(l, active ? lv_color_hex(0xFFFFFF) : lv_color_hex(CLR_CYAN), 0);
    }

    /* 阈值滑条 + 数值（OFF 时禁用；AP 源 1-200；ICM 温度 200-600=20-60℃，其余 1-500） */
    if (g_warn_thr_sl) {
        if (zc->src == WARN_SRC_NONE) {
            lv_obj_add_state(g_warn_thr_sl, LV_STATE_DISABLED);
        } else {
            lv_obj_clear_state(g_warn_thr_sl, LV_STATE_DISABLED);
            int max = 500;
            if (zc->src == WARN_SRC_AP3216C) max = 200;
            else if (zc->channel == IMU_DATA_TEMP) max = 600;
            lv_slider_set_range(g_warn_thr_sl, 1, max);
            lv_slider_set_value(g_warn_thr_sl, zc->threshold, LV_ANIM_OFF);
        }
    }
    if (g_warn_thr_lbl) {
        char b[8];
        snprintf(b, sizeof(b), "%d", zc->threshold);
        lv_label_set_text(g_warn_thr_lbl, b);
    }

    /* C2 双阈值：预警滑条（0=关闭，达到只轻提醒，不触发告警） */
    if (g_warn_pre_sl) {
        if (zc->src == WARN_SRC_NONE) {
            lv_obj_add_state(g_warn_pre_sl, LV_STATE_DISABLED);
        } else {
            lv_obj_clear_state(g_warn_pre_sl, LV_STATE_DISABLED);
            int max = 500;
            if (zc->src == WARN_SRC_AP3216C) max = 200;
            else if (zc->channel == IMU_DATA_TEMP) max = 600;
            lv_slider_set_range(g_warn_pre_sl, 0, max);
            lv_slider_set_value(g_warn_pre_sl, zc->threshold_warn, LV_ANIM_OFF);
        }
    }
    if (g_warn_pre_lbl) {
        char b[8];
        if (zc->threshold_warn > 0)
            snprintf(b, sizeof(b), "%d", zc->threshold_warn);
        else
            snprintf(b, sizeof(b), "OFF");
        lv_label_set_text(g_warn_pre_lbl, b);
    }
}

static void _on_sched_start_cb(lv_event_t *e)
{
    lv_obj_t *sl = lv_event_get_target(e);
    g_sched_start_h = (int)lv_slider_get_value(sl);
    if (g_sched_start_lbl) {
        char b[8];
        snprintf(b, sizeof(b), "%02d:00", g_sched_start_h);
        lv_label_set_text(g_sched_start_lbl, b);
    }
    _cfg_save_ui();
}

static void _on_sched_end_cb(lv_event_t *e)
{
    lv_obj_t *sl = lv_event_get_target(e);
    g_sched_end_h = (int)lv_slider_get_value(sl);
    if (g_sched_end_lbl) {
        char b[8];
        snprintf(b, sizeof(b), "%02d:00", g_sched_end_h);
        lv_label_set_text(g_sched_end_lbl, b);
    }
    _cfg_save_ui();
}

static void _on_sched_toggle(lv_event_t *e)
{
    (void)e;
    g_schedule_on = !g_schedule_on;
    if (g_schedule_on) {
        if (g_sched_toggle)
            lv_obj_set_style_bg_color(g_sched_toggle, lv_color_hex(CLR_CYAN), 0);
        if (g_sched_knob) lv_obj_align(g_sched_knob, LV_ALIGN_RIGHT_MID, -2, 0);
        if (g_sched_hint) {
            char b[32];
            snprintf(b, sizeof(b), "定时布防 %02d:00-%02d:00 已设",
                     g_sched_start_h, g_sched_end_h);
            lv_label_set_text(g_sched_hint, b);
            lv_obj_clear_flag(g_sched_hint, LV_OBJ_FLAG_HIDDEN);
        }
        /* 启用记录事件 */
        _add_event("定时布防已设",
                   "系统", CLR_CYAN, "");
    } else {
        if (g_sched_toggle)
            lv_obj_set_style_bg_color(g_sched_toggle, lv_color_hex(CLR_BORDER), 0);
        if (g_sched_knob) lv_obj_align(g_sched_knob, LV_ALIGN_LEFT_MID, 2, 0);
        if (g_sched_hint) lv_obj_add_flag(g_sched_hint, LV_OBJ_FLAG_HIDDEN);
        _add_event("定时布防已关",
                   "系统", CLR_CYAN, "");
    }
    _cfg_save_ui();
}

static void _build_settings_modal(void)
{
    s_overlay = uiw_modal_create(lv_screen_active(),
                                           "系统设置", 440);
    if (!s_overlay) return;

    lv_obj_t *box = lv_obj_get_child(s_overlay, 0);
    if (!box) return;

    lv_obj_t *body = lv_obj_get_child(box, 1);
    if (!body) return;

    /* 固定弹窗大小：box 高度固定 500（不随内容变化），body 内部滚动 */
    lv_obj_set_height(box, 500);
    lv_obj_set_style_max_height(body, 452, 0);

    /* --- 显示设置 --- */
    uiw_label(body, "画面设置", CLR_TEXT_DIM);
    {
        lv_obj_t *row = uiw_obj(body);
        if (row) {
            lv_obj_set_size(row, 368, LV_SIZE_CONTENT);
            lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                                  LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_top(row, 6, 0);
            lv_obj_set_style_pad_bottom(row, 6, 0);
            uiw_label(row, "亮度", CLR_TEXT_LO);
            g_bright_lbl = uiw_label_font(row, "80%", CLR_CYAN, &lv_font_SHSC_16);
        }
        /* 亮度滑条（HTML input[type=range]）— 同步 LCD 背光
         * 亮度以持久化配置为准（重启保持）；配置异常时才读当前背光兜底 */
        if (g_brightness < 0 || g_brightness > 100)
            g_brightness = _backlight_get();
        if (g_bright_lbl) {
            char b[8];
            snprintf(b, sizeof(b), "%d%%", g_brightness);
            lv_label_set_text(g_bright_lbl, b);
        }
        lv_obj_t *sl = lv_slider_create(body);
        if (sl) {
            lv_obj_set_width(sl, 368);
            lv_obj_set_height(sl, 20);
            lv_slider_set_range(sl, 0, 100);
            lv_slider_set_value(sl, g_brightness, LV_ANIM_OFF);
            /* 轨道 */
            lv_obj_set_style_bg_color(sl, lv_color_hex(CLR_BORDER), LV_PART_MAIN);
            lv_obj_set_style_bg_opa(sl, LV_OPA_COVER, LV_PART_MAIN);
            /* 已填充部分 */
            lv_obj_set_style_bg_color(sl, lv_color_hex(CLR_CYAN), LV_PART_INDICATOR);
            lv_obj_set_style_bg_opa(sl, LV_OPA_COVER, LV_PART_INDICATOR);
            /* 滑块圆钮 */
            lv_obj_set_style_bg_color(sl, lv_color_hex(CLR_CYAN), LV_PART_KNOB);
            lv_obj_set_style_bg_opa(sl, LV_OPA_COVER, LV_PART_KNOB);
            lv_obj_set_style_radius(sl, LV_RADIUS_CIRCLE, LV_PART_KNOB);
            lv_obj_set_style_shadow_color(sl, lv_color_hex(CLR_CYAN), LV_PART_KNOB);
            lv_obj_set_style_shadow_width(sl, 6, LV_PART_KNOB);
            lv_obj_set_style_shadow_opa(sl, LV_OPA_40, LV_PART_KNOB);
            lv_obj_add_event_cb(sl, _on_brightness_cb, LV_EVENT_VALUE_CHANGED, NULL);
        }
    }

    /* --- 检测设置（运动粗判参数，08-29：帧差间隔可设 + 灰度差阈值可设） --- */
    uiw_label(body, "检测设置", CLR_TEXT_DIM);
    {
        lv_obj_t *row = uiw_obj(body);
        if (row) {
            lv_obj_set_size(row, 368, LV_SIZE_CONTENT);
            lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                                  LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_top(row, 6, 0);
            uiw_label(row, "帧差间隔", CLR_TEXT_LO);
            g_itv_lbl = uiw_label_font(row, "", CLR_CYAN, &lv_font_SHSC_16);
        }
        lv_obj_t *btns = uiw_obj(body);
        if (btns) {
            lv_obj_set_size(btns, 368, LV_SIZE_CONTENT);
            lv_obj_set_flex_flow(btns, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(btns, LV_FLEX_ALIGN_END,
                                  LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_gap(btns, 10, 0);
            lv_obj_set_style_pad_bottom(btns, 6, 0);
            _step_btn(btns, "-", _on_itv_minus);
            _step_btn(btns, "+", _on_itv_plus);
        }
    }
    {
        lv_obj_t *row = uiw_obj(body);
        if (row) {
            lv_obj_set_size(row, 368, LV_SIZE_CONTENT);
            lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                                  LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            uiw_label(row, "灰度差阈值", CLR_TEXT_LO);
            g_diff_lbl = uiw_label_font(row, "", CLR_CYAN, &lv_font_SHSC_16);
        }
        lv_obj_t *btns = uiw_obj(body);
        if (btns) {
            lv_obj_set_size(btns, 368, LV_SIZE_CONTENT);
            lv_obj_set_flex_flow(btns, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(btns, LV_FLEX_ALIGN_END,
                                  LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_gap(btns, 10, 0);
            lv_obj_set_style_pad_bottom(btns, 10, 0);
            _step_btn(btns, "-", _on_diff_minus);
            _step_btn(btns, "+", _on_diff_plus);
        }
    }
    _motion_param_refresh();

    /* --- 网络状态 --- */
    uiw_label(body, "网络状态", CLR_TEXT_DIM);
    {
        const char *net_rows[][2] = {
            {"网络", "在线"},
            {"IP", "192.168.1.10"},
            {"MQTT 主机", "broker.local:1883"},
        };
        for (int r = 0; r < 3; r++) {
            lv_obj_t *row = uiw_obj(body);
            if (!row) continue;
            lv_obj_set_size(row, 368, LV_SIZE_CONTENT);
            lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                                  LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_top(row, 6, 0);
            lv_obj_set_style_pad_bottom(row, 6, 0);
            uiw_label(row, net_rows[r][0], CLR_TEXT_LO);
            uiw_label_font(row, net_rows[r][1], CLR_CYAN, &lv_font_SHSC_16);
        }
    }

    /* --- 告警分级 --- */
    uiw_label(body, "警级", CLR_TEXT_DIM);
    {
        /* HTML 原型完整描述：高=入侵检测、异常闯入 / 中=信号弱、设备离线 / 低=温度偏高、存储不足 */
        struct { const char *name; uint32_t clr; const char *desc; } levels[] = {
            {"高", CLR_RED,   "入侵检测"},
            {"中", CLR_AMBER, "部件离线"},
            {"L", CLR_CYAN,  "存量"},
        };
        for (int r = 0; r < 3; r++) {
            lv_obj_t *row = uiw_obj(body);
            if (!row) continue;
            lv_obj_set_size(row, 368, LV_SIZE_CONTENT);
            lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START,
                                  LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_gap(row, 8, 0);
            lv_obj_set_style_pad_top(row, 6, 0);
            lv_obj_set_style_pad_bottom(row, 6, 0);
            uiw_dot(row, levels[r].clr, 8);
            uiw_label(row, levels[r].name, CLR_TEXT_HI);
            uiw_label(row, levels[r].desc, CLR_TEXT_DIM);
        }
    }

    /* --- 识别设置（云端复核开关：开=两级管线 本地初判+云端复核定案；关=纯本地） --- */
    uiw_label(body, "识别设置", CLR_TEXT_DIM);
    {
        lv_obj_t *dm_row = uiw_obj(body);
        if (dm_row) {
            lv_obj_set_size(dm_row, 368, LV_SIZE_CONTENT);
            lv_obj_set_flex_flow(dm_row, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(dm_row, LV_FLEX_ALIGN_START,
                                  LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_gap(dm_row, 8, 0);
            lv_obj_set_style_pad_bottom(dm_row, 4, 0);
            uiw_label(dm_row, "REVIEW", CLR_TEXT_DIM);
            lv_obj_t *b = lv_button_create(dm_row);
            if (b) {
                lv_obj_set_size(b, LV_SIZE_CONTENT, 28);
                lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, 0);
                lv_obj_set_style_border_color(b, lv_color_hex(CLR_BORDER), 0);
                lv_obj_set_style_border_width(b, 1, 0);
                lv_obj_set_style_radius(b, 0, 0);
                lv_obj_set_style_pad_left(b, 10, 0);
                lv_obj_set_style_pad_right(b, 10, 0);
                lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
                lv_obj_t *l = uiw_label(b, "云端复核：开", CLR_CYAN);
                if (l) lv_obj_center(l);
                lv_obj_add_event_cb(b, _on_review_toggle, LV_EVENT_CLICKED, NULL);
                g_dm_btn = b;
            }
        }
        _review_refresh();   /* 初始文案/高亮 + 徽章联动 */
    }

    /* --- 预警设置（四位置 × 触发源 × 数据通道 × 阈值） --- */
    uiw_label(body, "预警设置", CLR_TEXT_DIM);
    {
        /* 单区域：固定为摄像头归属防区，不再提供区域切换 */
        lv_obj_t *zone_row = uiw_obj(body);
        if (zone_row) {
            lv_obj_set_size(zone_row, 368, LV_SIZE_CONTENT);
            lv_obj_set_flex_flow(zone_row, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(zone_row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                                  LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_top(zone_row, 4, 0);
            lv_obj_set_style_pad_bottom(zone_row, 8, 0);
            uiw_label(zone_row, "区域", CLR_TEXT_DIM);
            {
                int cz = config_get()->camera_zone;
                if (cz < 0 || cz >= ZONE_COUNT) cz = 0;
                uiw_label(zone_row, zone_info[cz].name, CLR_CYAN);
            }
        }

        /* 触发源行：OFF / AP3216C / ICM */
        lv_obj_t *src_row = uiw_obj(body);
        if (src_row) {
            lv_obj_set_size(src_row, 368, LV_SIZE_CONTENT);
            lv_obj_set_flex_flow(src_row, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(src_row, LV_FLEX_ALIGN_START,
                                  LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_gap(src_row, 8, 0);
            lv_obj_set_style_pad_bottom(src_row, 4, 0);
            uiw_label(src_row, "SRC", CLR_TEXT_DIM);
            const char *src_names[3] = { "OFF", "AP3216C", "ICM" };
            for (int i = 0; i < 3; i++) {
                lv_obj_t *b = lv_button_create(src_row);
                if (!b) continue;
                lv_obj_set_size(b, LV_SIZE_CONTENT, 28);
                lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, 0);
                lv_obj_set_style_border_color(b, lv_color_hex(CLR_BORDER), 0);
                lv_obj_set_style_border_width(b, 1, 0);
                lv_obj_set_style_radius(b, 0, 0);
                lv_obj_set_style_pad_left(b, 10, 0);
                lv_obj_set_style_pad_right(b, 10, 0);
                lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
                lv_obj_t *l = uiw_label(b, src_names[i], CLR_CYAN);
                if (l) lv_obj_center(l);
                lv_obj_add_event_cb(b, _on_warn_src_sel, LV_EVENT_CLICKED,
                                   (void *)(intptr_t)i);
                g_warn_src_btns[i] = b;
            }
        }

        /* 数据通道行：PS/ALS/IR 或 MAG/AX/AY/AZ（随源变化） */
        lv_obj_t *ch_row = uiw_obj(body);
        if (ch_row) {
            lv_obj_set_size(ch_row, 368, LV_SIZE_CONTENT);
            lv_obj_set_flex_flow(ch_row, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(ch_row, LV_FLEX_ALIGN_START,
                                  LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_gap(ch_row, 8, 0);
            lv_obj_set_style_pad_bottom(ch_row, 4, 0);
            uiw_label(ch_row, "DATA", CLR_TEXT_DIM);
            /* 按钮名按源复用：AP3216C 用 PS/ALS/IR；ICM 用 LIN/AX/AY/AZ/TEMP */
            const char *ch_names[5] = { "PS", "ALS", "IR", "LIN", "TEMP" };
            for (int i = 0; i < 5; i++) {
                lv_obj_t *b = lv_button_create(ch_row);
                if (!b) continue;
                lv_obj_set_size(b, LV_SIZE_CONTENT, 28);
                lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, 0);
                lv_obj_set_style_border_color(b, lv_color_hex(CLR_BORDER), 0);
                lv_obj_set_style_border_width(b, 1, 0);
                lv_obj_set_style_radius(b, 0, 0);
                lv_obj_set_style_pad_left(b, 10, 0);
                lv_obj_set_style_pad_right(b, 10, 0);
                lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
                lv_obj_t *l = uiw_label(b, ch_names[i], CLR_CYAN);
                if (l) lv_obj_center(l);
                lv_obj_add_event_cb(b, _on_warn_ch_sel, LV_EVENT_CLICKED,
                                   (void *)(intptr_t)i);
                g_warn_ch_btns[i] = b;
            }
        }

        /* 比较方式行：GT(大于) / EQ(等于) / LT(小于) */
        lv_obj_t *cmp_row = uiw_obj(body);
        if (cmp_row) {
            lv_obj_set_size(cmp_row, 368, LV_SIZE_CONTENT);
            lv_obj_set_flex_flow(cmp_row, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(cmp_row, LV_FLEX_ALIGN_START,
                                  LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_gap(cmp_row, 8, 0);
            lv_obj_set_style_pad_bottom(cmp_row, 4, 0);
            uiw_label(cmp_row, "CMP", CLR_TEXT_DIM);
            const char *cmp_names[3] = { "GT", "EQ", "LT" };
            for (int i = 0; i < 3; i++) {
                lv_obj_t *b = lv_button_create(cmp_row);
                if (!b) continue;
                lv_obj_set_size(b, LV_SIZE_CONTENT, 28);
                lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, 0);
                lv_obj_set_style_border_color(b, lv_color_hex(CLR_BORDER), 0);
                lv_obj_set_style_border_width(b, 1, 0);
                lv_obj_set_style_radius(b, 0, 0);
                lv_obj_set_style_pad_left(b, 10, 0);
                lv_obj_set_style_pad_right(b, 10, 0);
                lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
                lv_obj_t *l = uiw_label(b, cmp_names[i], CLR_CYAN);
                if (l) lv_obj_center(l);
                lv_obj_add_event_cb(b, _on_warn_cmp_sel, LV_EVENT_CLICKED,
                                   (void *)(intptr_t)i);
                g_warn_cmp_btns[i] = b;
            }
        }

        /* 阈值行 + 滑条 */
        lv_obj_t *thr_row = uiw_obj(body);
        if (thr_row) {
            lv_obj_set_size(thr_row, 368, LV_SIZE_CONTENT);
            lv_obj_set_flex_flow(thr_row, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(thr_row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                                  LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_top(thr_row, 6, 0);
            lv_obj_set_style_pad_bottom(thr_row, 4, 0);
            uiw_label(thr_row, "LIMIT", CLR_TEXT_DIM);
            g_warn_thr_lbl = uiw_label_font(thr_row, "30", CLR_CYAN, &lv_font_SHSC_16);
        }
        g_warn_thr_sl = lv_slider_create(body);
        if (g_warn_thr_sl) {
            lv_obj_set_width(g_warn_thr_sl, 368);
            lv_obj_set_height(g_warn_thr_sl, 20);
            lv_slider_set_range(g_warn_thr_sl, 1, 200);
            lv_obj_set_style_bg_color(g_warn_thr_sl, lv_color_hex(CLR_BORDER), LV_PART_MAIN);
            lv_obj_set_style_bg_opa(g_warn_thr_sl, LV_OPA_COVER, LV_PART_MAIN);
            lv_obj_set_style_bg_color(g_warn_thr_sl, lv_color_hex(CLR_CYAN), LV_PART_INDICATOR);
            lv_obj_set_style_bg_opa(g_warn_thr_sl, LV_OPA_COVER, LV_PART_INDICATOR);
            lv_obj_set_style_bg_color(g_warn_thr_sl, lv_color_hex(CLR_CYAN), LV_PART_KNOB);
            lv_obj_set_style_bg_opa(g_warn_thr_sl, LV_OPA_COVER, LV_PART_KNOB);
            lv_obj_set_style_radius(g_warn_thr_sl, LV_RADIUS_CIRCLE, LV_PART_KNOB);
            lv_obj_set_style_shadow_color(g_warn_thr_sl, lv_color_hex(CLR_CYAN), LV_PART_KNOB);
            lv_obj_set_style_shadow_width(g_warn_thr_sl, 6, LV_PART_KNOB);
            lv_obj_set_style_shadow_opa(g_warn_thr_sl, LV_OPA_40, LV_PART_KNOB);
            lv_obj_add_event_cb(g_warn_thr_sl, _on_warn_thr_cb, LV_EVENT_VALUE_CHANGED, NULL);
        }

        /* C2 双阈值：预警阈值（0=OFF）行 + 滑条 */
        lv_obj_t *pre_row = uiw_obj(body);
        if (pre_row) {
            lv_obj_set_size(pre_row, 368, LV_SIZE_CONTENT);
            lv_obj_set_flex_flow(pre_row, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(pre_row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                                  LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_top(pre_row, 6, 0);
            lv_obj_set_style_pad_bottom(pre_row, 4, 0);
            uiw_label(pre_row, "PRE", CLR_TEXT_DIM);
            g_warn_pre_lbl = uiw_label_font(pre_row, "OFF", CLR_AMBER, &lv_font_SHSC_16);
        }
        g_warn_pre_sl = lv_slider_create(body);
        if (g_warn_pre_sl) {
            lv_obj_set_width(g_warn_pre_sl, 368);
            lv_obj_set_height(g_warn_pre_sl, 20);
            lv_slider_set_range(g_warn_pre_sl, 0, 200);
            lv_obj_set_style_bg_color(g_warn_pre_sl, lv_color_hex(CLR_BORDER), LV_PART_MAIN);
            lv_obj_set_style_bg_opa(g_warn_pre_sl, LV_OPA_COVER, LV_PART_MAIN);
            lv_obj_set_style_bg_color(g_warn_pre_sl, lv_color_hex(CLR_AMBER), LV_PART_INDICATOR);
            lv_obj_set_style_bg_opa(g_warn_pre_sl, LV_OPA_COVER, LV_PART_INDICATOR);
            lv_obj_set_style_bg_color(g_warn_pre_sl, lv_color_hex(CLR_AMBER), LV_PART_KNOB);
            lv_obj_set_style_bg_opa(g_warn_pre_sl, LV_OPA_COVER, LV_PART_KNOB);
            lv_obj_set_style_radius(g_warn_pre_sl, LV_RADIUS_CIRCLE, LV_PART_KNOB);
            lv_obj_set_style_shadow_color(g_warn_pre_sl, lv_color_hex(CLR_AMBER), LV_PART_KNOB);
            lv_obj_set_style_shadow_width(g_warn_pre_sl, 6, LV_PART_KNOB);
            lv_obj_set_style_shadow_opa(g_warn_pre_sl, LV_OPA_40, LV_PART_KNOB);
            lv_obj_add_event_cb(g_warn_pre_sl, _on_warn_pre_cb, LV_EVENT_VALUE_CHANGED, NULL);
        }

        _warn_refresh_ui();   /* 初始高亮（位置0） */

        /* 消警冷却时间行 + 滑条 */
        struct dev_config cfg_cd;
        dev_bridge_get_config(&cfg_cd);
        lv_obj_t *cd_row = uiw_obj(body);
        if (cd_row) {
            lv_obj_set_size(cd_row, 368, LV_SIZE_CONTENT);
            lv_obj_set_flex_flow(cd_row, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(cd_row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                                  LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_top(cd_row, 6, 0);
            lv_obj_set_style_pad_bottom(cd_row, 4, 0);
            uiw_label(cd_row, "COOLDOWN", CLR_TEXT_DIM);
            g_cool_lbl = uiw_label_font(cd_row, "60s", CLR_CYAN, &lv_font_SHSC_16);
        }
        lv_obj_t *cd_sl = lv_slider_create(body);
        if (cd_sl) {
            lv_obj_set_width(cd_sl, 368);
            lv_obj_set_height(cd_sl, 20);
            lv_slider_set_range(cd_sl, 0, 300);   /* 0-300 秒 */
            lv_slider_set_value(cd_sl, cfg_cd.cooldown_sec, LV_ANIM_OFF);
            lv_obj_set_style_bg_color(cd_sl, lv_color_hex(CLR_BORDER), LV_PART_MAIN);
            lv_obj_set_style_bg_opa(cd_sl, LV_OPA_COVER, LV_PART_MAIN);
            lv_obj_set_style_bg_color(cd_sl, lv_color_hex(CLR_CYAN), LV_PART_INDICATOR);
            lv_obj_set_style_bg_opa(cd_sl, LV_OPA_COVER, LV_PART_INDICATOR);
            lv_obj_set_style_bg_color(cd_sl, lv_color_hex(CLR_CYAN), LV_PART_KNOB);
            lv_obj_set_style_bg_opa(cd_sl, LV_OPA_COVER, LV_PART_KNOB);
            lv_obj_set_style_radius(cd_sl, LV_RADIUS_CIRCLE, LV_PART_KNOB);
            lv_obj_set_style_shadow_color(cd_sl, lv_color_hex(CLR_CYAN), LV_PART_KNOB);
            lv_obj_set_style_shadow_width(cd_sl, 6, LV_PART_KNOB);
            lv_obj_set_style_shadow_opa(cd_sl, LV_OPA_40, LV_PART_KNOB);
            lv_obj_add_event_cb(cd_sl, _on_cool_cb, LV_EVENT_VALUE_CHANGED, NULL);
        }
    }

    /* --- 定时布防（时间段可设 + 开关） --- */
    uiw_label(body, "定时布防", CLR_TEXT_DIM);
    {
        lv_obj_t *row = uiw_obj(body);
        if (row) {
            lv_obj_set_size(row, 368, LV_SIZE_CONTENT);
            lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                                  LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_top(row, 6, 0);
            lv_obj_set_style_pad_bottom(row, 6, 0);
            uiw_label(row, "定时布防", CLR_TEXT_LO);

            /* 可点击开关（HTML .toggle-switch），初始 OFF：灰底 + 滑块在左 */
            g_sched_toggle = uiw_obj(row);
            if (g_sched_toggle) {
                lv_obj_set_size(g_sched_toggle, 40, 20);
                lv_obj_set_style_bg_color(g_sched_toggle, lv_color_hex(CLR_BORDER), 0);
                lv_obj_set_style_bg_opa(g_sched_toggle, LV_OPA_COVER, 0);
                lv_obj_set_style_radius(g_sched_toggle, 10, 0);
                lv_obj_set_style_border_width(g_sched_toggle, 0, 0);
                lv_obj_add_flag(g_sched_toggle, LV_OBJ_FLAG_CLICKABLE);
                lv_obj_add_event_cb(g_sched_toggle, _on_sched_toggle, LV_EVENT_CLICKED, NULL);

                /* 滑块 */
                g_sched_knob = uiw_obj(g_sched_toggle);
                if (g_sched_knob) {
                    lv_obj_set_size(g_sched_knob, 16, 16);
                    lv_obj_set_style_bg_color(g_sched_knob, lv_color_hex(0xFFFFFF), 0);
                    lv_obj_set_style_bg_opa(g_sched_knob, LV_OPA_COVER, 0);
                    lv_obj_set_style_radius(g_sched_knob, LV_RADIUS_CIRCLE, 0);
                    lv_obj_set_style_border_width(g_sched_knob, 0, 0);
                    lv_obj_align(g_sched_knob, LV_ALIGN_LEFT_MID, 2, 0);
                }
            }
        }

        /* 开始时间行 */
        lv_obj_t *srow = uiw_obj(body);
        if (srow) {
            lv_obj_set_size(srow, 368, LV_SIZE_CONTENT);
            lv_obj_set_flex_flow(srow, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(srow, LV_FLEX_ALIGN_SPACE_BETWEEN,
                                  LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_top(srow, 6, 0);
            lv_obj_set_style_pad_bottom(srow, 4, 0);
            uiw_label(srow, "START", CLR_TEXT_DIM);
            g_sched_start_lbl = uiw_label_font(srow, "22:00", CLR_AMBER, &lv_font_SHSC_16);
            if (g_sched_start_lbl) {
                char b[8];
                snprintf(b, sizeof(b), "%02d:00", g_sched_start_h);
                lv_label_set_text(g_sched_start_lbl, b);
            }
        }
        lv_obj_t *ssl = lv_slider_create(body);
        if (ssl) {
            lv_obj_set_width(ssl, 368);
            lv_obj_set_height(ssl, 20);
            lv_slider_set_range(ssl, 0, 23);
            lv_slider_set_value(ssl, g_sched_start_h, LV_ANIM_OFF);
            lv_obj_set_style_bg_color(ssl, lv_color_hex(CLR_BORDER), LV_PART_MAIN);
            lv_obj_set_style_bg_opa(ssl, LV_OPA_COVER, LV_PART_MAIN);
            lv_obj_set_style_bg_color(ssl, lv_color_hex(CLR_AMBER), LV_PART_INDICATOR);
            lv_obj_set_style_bg_opa(ssl, LV_OPA_COVER, LV_PART_INDICATOR);
            lv_obj_set_style_bg_color(ssl, lv_color_hex(CLR_AMBER), LV_PART_KNOB);
            lv_obj_set_style_bg_opa(ssl, LV_OPA_COVER, LV_PART_KNOB);
            lv_obj_set_style_radius(ssl, LV_RADIUS_CIRCLE, LV_PART_KNOB);
            lv_obj_set_style_shadow_color(ssl, lv_color_hex(CLR_AMBER), LV_PART_KNOB);
            lv_obj_set_style_shadow_width(ssl, 6, LV_PART_KNOB);
            lv_obj_set_style_shadow_opa(ssl, LV_OPA_40, LV_PART_KNOB);
            lv_obj_add_event_cb(ssl, _on_sched_start_cb, LV_EVENT_VALUE_CHANGED, NULL);
        }

        /* 结束时间行 */
        lv_obj_t *erow = uiw_obj(body);
        if (erow) {
            lv_obj_set_size(erow, 368, LV_SIZE_CONTENT);
            lv_obj_set_flex_flow(erow, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(erow, LV_FLEX_ALIGN_SPACE_BETWEEN,
                                  LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_top(erow, 6, 0);
            lv_obj_set_style_pad_bottom(erow, 4, 0);
            uiw_label(erow, "END", CLR_TEXT_DIM);
            g_sched_end_lbl = uiw_label_font(erow, "06:00", CLR_AMBER, &lv_font_SHSC_16);
            if (g_sched_end_lbl) {
                char b[8];
                snprintf(b, sizeof(b), "%02d:00", g_sched_end_h);
                lv_label_set_text(g_sched_end_lbl, b);
            }
        }
        lv_obj_t *esl = lv_slider_create(body);
        if (esl) {
            lv_obj_set_width(esl, 368);
            lv_obj_set_height(esl, 20);
            lv_slider_set_range(esl, 0, 23);
            lv_slider_set_value(esl, g_sched_end_h, LV_ANIM_OFF);
            lv_obj_set_style_bg_color(esl, lv_color_hex(CLR_BORDER), LV_PART_MAIN);
            lv_obj_set_style_bg_opa(esl, LV_OPA_COVER, LV_PART_MAIN);
            lv_obj_set_style_bg_color(esl, lv_color_hex(CLR_AMBER), LV_PART_INDICATOR);
            lv_obj_set_style_bg_opa(esl, LV_OPA_COVER, LV_PART_INDICATOR);
            lv_obj_set_style_bg_color(esl, lv_color_hex(CLR_AMBER), LV_PART_KNOB);
            lv_obj_set_style_bg_opa(esl, LV_OPA_COVER, LV_PART_KNOB);
            lv_obj_set_style_radius(esl, LV_RADIUS_CIRCLE, LV_PART_KNOB);
            lv_obj_set_style_shadow_color(esl, lv_color_hex(CLR_AMBER), LV_PART_KNOB);
            lv_obj_set_style_shadow_width(esl, 6, LV_PART_KNOB);
            lv_obj_set_style_shadow_opa(esl, LV_OPA_40, LV_PART_KNOB);
            lv_obj_add_event_cb(esl, _on_sched_end_cb, LV_EVENT_VALUE_CHANGED, NULL);
        }
    }

    /* 提示文字（默认隐藏，开启时显示） */
    g_sched_hint = uiw_label(body, "定时布防 "
                              "22:00-06:00 "
                              "已设",
                              CLR_AMBER);
    if (g_sched_hint) lv_obj_add_flag(g_sched_hint, LV_OBJ_FLAG_HIDDEN);

    /* 用持久化配置恢复定时布防初始状态（开关样式/滑块位置/提示文案） */
    if (g_schedule_on) {
        if (g_sched_toggle)
            lv_obj_set_style_bg_color(g_sched_toggle, lv_color_hex(CLR_CYAN), 0);
        if (g_sched_knob)
            lv_obj_align(g_sched_knob, LV_ALIGN_RIGHT_MID, -2, 0);
        if (g_sched_hint) {
            char b[32];
            snprintf(b, sizeof(b), "定时布防 %02d:00-%02d:00 已设",
                     g_sched_start_h, g_sched_end_h);
            lv_label_set_text(g_sched_hint, b);
            lv_obj_clear_flag(g_sched_hint, LV_OBJ_FLAG_HIDDEN);
        }
    }

    /* --- 关于 --- */
    uiw_label(body, "系统", CLR_TEXT_DIM);
    {
        const char *about[][2] = {
            {"主机", "i.MX6ULL 安防网关"},
            {"系统", "v2.3.1"},
            {"系统", "Linux 5.4.0"},
            {"存量", "14.2GB / 32GB"},
        };
        for (int r = 0; r < 4; r++) {
            lv_obj_t *row = uiw_obj(body);
            if (!row) continue;
            lv_obj_set_size(row, 368, LV_SIZE_CONTENT);
            lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                                  LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_top(row, 6, 0);
            lv_obj_set_style_pad_bottom(row, 6, 0);
            lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
            lv_obj_set_style_border_color(row, lv_color_hex(CLR_BORDER), 0);
            lv_obj_set_style_border_width(row, 1, 0);
            uiw_label(row, about[r][0], CLR_TEXT_LO);
            uiw_label_font(row, about[r][1], CLR_CYAN, &lv_font_SHSC_16);
        }
    }

    /* --- 维护：重启应用 --- */
    uiw_label(body, "维护", CLR_TEXT_DIM);
    {
        lv_obj_t *btn = lv_button_create(body);
        if (btn) {
            lv_obj_set_size(btn, 368, 42);
            lv_obj_set_style_bg_color(btn, lv_color_hex(CLR_RED), 0);
            lv_obj_set_style_bg_opa(btn, LV_OPA_10, 0);
            lv_obj_set_style_border_color(btn, lv_color_hex(CLR_RED), 0);
            lv_obj_set_style_border_width(btn, 1, 0);
            lv_obj_set_style_radius(btn, 4, 0);
            lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_t *l = uiw_label_font(btn, "重启应用", CLR_RED, FONT_UI);
            if (l) lv_obj_center(l);
            lv_obj_add_event_cb(btn, _on_restart_app, LV_EVENT_CLICKED, NULL);
        }
    }
}

static void _on_restart_app(lv_event_t *e)
{
    (void)e;
    _show_toast("正在重启应用");
    health_request_restart();
}

/* ================================================================
 * 对外接口
 * ================================================================ */
void ui_settings_build(void)
{
    _build_settings_modal();
}

void ui_settings_open(void)
{
    if (s_overlay)
        lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    _add_event("打开设置界面", "系统", CLR_CYAN, "");
}

int ui_settings_visible(void)
{
    return s_overlay && !lv_obj_has_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
}

/* 云端复核按钮视觉（徽章由 ui_home 的 _review_refresh 统一驱动） */
void ui_settings_refresh_review(int on)
{
    if (!g_dm_btn) return;
    lv_obj_t *l = lv_obj_get_child(g_dm_btn, 0);
    lv_obj_set_style_bg_opa(g_dm_btn, on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    if (l) {
        lv_obj_set_style_text_color(l, on ? lv_color_hex(0xFFFFFF) : lv_color_hex(CLR_CYAN), 0);
        lv_label_set_text(l, on ? "云端复核：开" : "云端复核：关");
    }
}

void ui_settings_refresh_motion(void)
{
    char b[16];
    if (g_itv_lbl) {
        snprintf(b, sizeof(b), "%d 帧", cam_feed_get_motion_interval());
        lv_label_set_text(g_itv_lbl, b);
    }
    if (g_diff_lbl) {
        snprintf(b, sizeof(b), "%d", cam_feed_get_motion_diff());
        lv_label_set_text(g_diff_lbl, b);
    }
}

void ui_settings_sync_from_config(void)
{
    if (g_bright_lbl) {
        char b[8];
        snprintf(b, sizeof(b), "%d%%", g_brightness);
        lv_label_set_text(g_bright_lbl, b);
    }
    if (g_sched_start_lbl) {
        char b[8];
        snprintf(b, sizeof(b), "%02d:00", g_sched_start_h);
        lv_label_set_text(g_sched_start_lbl, b);
    }
    if (g_sched_end_lbl) {
        char b[8];
        snprintf(b, sizeof(b), "%02d:00", g_sched_end_h);
        lv_label_set_text(g_sched_end_lbl, b);
    }
}
