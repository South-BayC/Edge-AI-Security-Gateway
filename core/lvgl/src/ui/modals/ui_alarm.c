/**
 * ui_alarm.c — 告警确认弹窗实现
 */
#include "ui_alarm.h"
#include "../ui_widgets.h"
#include "../ui_theme.h"
#include "../ui_time.h"
#include "../../domain/zone_state.h"
#include "../../alarm_fsm.h"

#include <stdio.h>

static lv_obj_t *s_popup = NULL;       /* 弹窗遮罩 */
static lv_obj_t *s_info_lbl = NULL;    /* 告警信息行（区域/类型/时间） */
static lv_obj_t *s_confirm_bar = NULL; /* 长按进度条 */
static uint32_t s_confirm_tick = 0;    /* 长按起始 tick（0=未按住） */
static int s_alarm_zone = -1;

static ui_alarm_hooks_t s_hooks;

void ui_alarm_set_hooks(const ui_alarm_hooks_t *hooks)
{
    if (hooks)
        s_hooks = *hooks;
}

void ui_alarm_clear_fsm(int zone)
{
    alarm_fsm_dispatch(zone, ALARM_EV_ACK, "人工消警");
    alarm_fsm_dispatch(zone, ALARM_EV_CLEAR, "人工消警");
    alarm_fsm_dispatch(zone, ALARM_EV_TIMEOUT, "人工消警");
}

/* 长按进度（LV_EVENT_PRESSING 持续触发；松开复位） */
static void _on_confirm_pressing(lv_event_t *e)
{
    (void)e;
    if (s_confirm_tick == 0) s_confirm_tick = lv_tick_get();
    uint32_t held = lv_tick_elaps(s_confirm_tick);

    if (s_confirm_bar)
        lv_bar_set_value(s_confirm_bar, held >= 1000 ? 100 : held / 10, LV_ANIM_OFF);

    if (held >= 1000) {
        s_confirm_tick = 0;
        if (s_confirm_bar) lv_bar_set_value(s_confirm_bar, 100, LV_ANIM_OFF);

        /* 确认消警：消除所有告警分区（回布防中）+ 关弹窗 */
        for (int i = 0; i < ZONE_COUNT; i++) {
            if (zone_state_get(i) == ZONE_ALARM) {
                ui_alarm_clear_fsm(i);
                zone_state_ack(i);
                if (s_hooks.on_zone_ack) s_hooks.on_zone_ack(i);
            }
        }
        if (s_hooks.on_confirmed) s_hooks.on_confirmed();

        if (s_popup)
            lv_obj_add_flag(s_popup, LV_OBJ_FLAG_HIDDEN);
    }
}

static void _on_confirm_released(lv_event_t *e)
{
    (void)e;
    s_confirm_tick = 0;   /* 未满 1s 松开：复位进度 */
    if (s_confirm_bar)
        lv_bar_set_value(s_confirm_bar, 0, LV_ANIM_OFF);
}

/* 一键静音：蜂鸣器停（告警状态保留，消警后自动恢复跟随） */
static void _on_mute_clicked(lv_event_t *e)
{
    (void)e;
    if (s_hooks.on_muted) s_hooks.on_muted();
}

void ui_alarm_open(int zone_idx, const char *zone_name, const char *src)
{
    if (!s_popup) return;
    s_alarm_zone = zone_idx;

    if (s_info_lbl) {
        char t[8];
        ui_time_str(t, sizeof(t));
        char b[64];
        snprintf(b, sizeof(b), "%s · %s 触发 · %s",
                 zone_name ? zone_name : "",
                 (src && src[0]) ? src : "SENSOR", t);
        lv_label_set_text(s_info_lbl, b);
    }
    if (s_confirm_bar) lv_bar_set_value(s_confirm_bar, 0, LV_ANIM_OFF);
    s_confirm_tick = 0;

    lv_obj_clear_flag(s_popup, LV_OBJ_FLAG_HIDDEN);
}

int ui_alarm_visible(void)
{
    return s_popup && !lv_obj_has_flag(s_popup, LV_OBJ_FLAG_HIDDEN);
}

void ui_alarm_hide(void)
{
    if (s_popup)
        lv_obj_add_flag(s_popup, LV_OBJ_FLAG_HIDDEN);
}

void ui_alarm_build(void)
{
    s_popup = uiw_modal_create(lv_screen_active(), "告警", 440);
    if (!s_popup) return;

    lv_obj_t *box = lv_obj_get_child(s_popup, 0);
    if (!box) return;
    lv_obj_t *body = lv_obj_get_child(box, 1);
    if (!body) return;

    /* 告警信息行 */
    s_info_lbl = uiw_label_font(body, "--", CLR_RED, &lv_font_SHSC_16);
    if (s_info_lbl) {
        lv_obj_set_width(s_info_lbl, 368);
        lv_label_set_long_mode(s_info_lbl, LV_LABEL_LONG_DOT);
    }

    /* 长按消警按钮（带进度条指示按住时长） */
    lv_obj_t *btn = lv_button_create(body);
    if (btn) {
        lv_obj_set_size(btn, 368, 52);
        lv_obj_set_style_bg_color(btn, lv_color_hex(CLR_RED), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_30, 0);
        lv_obj_set_style_border_color(btn, lv_color_hex(CLR_RED), 0);
        lv_obj_set_style_border_width(btn, 1, 0);
        lv_obj_set_style_radius(btn, 0, 0);
        lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *bl = uiw_label(btn, "1s 消警（按住）", CLR_RED);
        if (bl) lv_obj_center(bl);

        lv_obj_add_event_cb(btn, _on_confirm_pressing, LV_EVENT_PRESSING, NULL);
        lv_obj_add_event_cb(btn, _on_confirm_released, LV_EVENT_RELEASED, NULL);
        lv_obj_add_event_cb(btn, _on_confirm_released, LV_EVENT_PRESS_LOST, NULL);
    }

    /* 长按进度条 */
    s_confirm_bar = lv_bar_create(body);
    if (s_confirm_bar) {
        lv_obj_set_size(s_confirm_bar, 368, 8);
        lv_bar_set_range(s_confirm_bar, 0, 100);
        lv_bar_set_value(s_confirm_bar, 0, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(s_confirm_bar, lv_color_hex(CLR_BORDER), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(s_confirm_bar, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_bg_color(s_confirm_bar, lv_color_hex(CLR_RED), LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(s_confirm_bar, LV_OPA_COVER, LV_PART_INDICATOR);
    }

    /* 一键静音 */
    lv_obj_t *mute = lv_button_create(body);
    if (mute) {
        lv_obj_set_size(mute, 368, 40);
        lv_obj_set_style_bg_color(mute, lv_color_hex(CLR_AMBER), 0);
        lv_obj_set_style_bg_opa(mute, LV_OPA_20, 0);
        lv_obj_set_style_border_color(mute, lv_color_hex(CLR_AMBER), 0);
        lv_obj_set_style_border_width(mute, 1, 0);
        lv_obj_set_style_radius(mute, 0, 0);
        lv_obj_clear_flag(mute, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t *ml = uiw_label(mute, "MUTE", CLR_AMBER);
        if (ml) lv_obj_center(ml);
        lv_obj_add_event_cb(mute, _on_mute_clicked, LV_EVENT_CLICKED, NULL);
    }
}
