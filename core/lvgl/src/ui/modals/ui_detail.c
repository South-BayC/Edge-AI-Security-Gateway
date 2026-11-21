/**
 * ui_detail.c — 分区详情弹窗实现
 */
#include "ui_detail.h"
#include "../ui_widgets.h"
#include "../ui_theme.h"
#include "../ui_internal.h"
#include "../ui_events.h"
#include "../../domain/zone_state.h"

#include <stdint.h>
#include <stdio.h>

static lv_obj_t *s_overlay = NULL;
static lv_obj_t *s_body = NULL;
static lv_obj_t *s_title_lbl = NULL;
static lv_obj_t *s_state_lbl = NULL;
static lv_obj_t *s_arm_btn = NULL;
static int s_zone_idx = -1;

/* 根据当前分区状态刷新弹窗内的状态行与按钮文字 */
static void _refresh_state(void)
{
    int idx = s_zone_idx;
    if (idx < 0 || idx >= ZONE_COUNT) return;

    zone_state_t st = zone_state_get(idx);
    const char *st_txt;
    uint32_t st_clr;

    switch (st) {
    case ZONE_ARMED: st_txt = "布防中";  st_clr = CLR_GREEN; break;
    case ZONE_ALARM: st_txt = "警";     st_clr = CLR_RED;   break;
    default:         st_txt = "已撤防"; st_clr = CLR_CYAN;  break;
    }

    if (s_state_lbl) {
        lv_label_set_text(s_state_lbl, st_txt);
        lv_obj_set_style_text_color(s_state_lbl, lv_color_hex(st_clr), 0);
    }

    /* 按钮文字与配色：布防中 → 显示"撤防"；否则 → 显示"布防" */
    if (s_arm_btn) {
        const char *txt = (st == ZONE_ARMED) ? "撤防" : "布防";
        uint32_t clr = (st == ZONE_ARMED) ? CLR_BORDER_LT : CLR_CYAN;
        lv_obj_set_style_border_color(s_arm_btn, lv_color_hex(clr), 0);
        lv_obj_t *l = lv_obj_get_child(s_arm_btn, 0);
        if (l) {
            lv_label_set_text(l, txt);
            lv_obj_set_style_text_color(l, lv_color_hex(clr), 0);
        }
    }
}

/* 状态设置按钮点击：切换该分区布防⇄撤防（告警中点击先消警） */
static void _on_arm_toggle(lv_event_t *e)
{
    (void)e;
    int idx = s_zone_idx;
    if (idx < 0 || idx >= ZONE_COUNT) return;

    switch (zone_state_get(idx)) {
    case ZONE_ALARM:
        ui_events_alarm_ack(idx);            /* 消警 → 回布防中 */
        break;
    case ZONE_ARMED:
        ui_events_zone_set_state(idx, ZONE_ONLINE);   /* 单独撤防 */
        _add_event("分区撤防", zone_info[idx].name, CLR_CYAN, "");
        break;
    default:
        ui_events_zone_set_state(idx, ZONE_ARMED);    /* 单独布防 */
        _add_event("分区布防", zone_info[idx].name, CLR_GREEN, "");
        break;
    }

    _refresh_state();
}

void ui_detail_card_click_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    ui_detail_open(idx);
}

void ui_detail_open(int zone_idx)
{
    if (zone_idx < 0 || zone_idx >= ZONE_COUNT || !s_body || !s_overlay)
        return;
    s_zone_idx = zone_idx;

    const zone_info_t *z = &zone_info[zone_idx];
    zone_state_t st = zone_state_get(zone_idx);
    const char *st_txt;
    uint32_t st_clr;

    switch (st) {
    case ZONE_ARMED: st_txt = "布防中";  st_clr = CLR_GREEN; break;
    case ZONE_ALARM: st_txt = "警";    st_clr = CLR_RED;   break;
    default:         st_txt = "已撤防"; st_clr = CLR_CYAN;  break;
    }

    /* 清空旧内容并重建 */
    lv_obj_clean(s_body);

    /* 动态更新弹窗标题："[分区名] 分区" */
    if (s_title_lbl) {
        char title_buf[32];
        snprintf(title_buf, sizeof(title_buf), "%s 分区", z->name);
        lv_label_set_text(s_title_lbl, title_buf);
    }

    /* 第一行：状态设置标签 + 布防/撤防按钮（分区级独立控制） */
    lv_obj_t *btn_row = uiw_obj(s_body);
    if (btn_row) {
        lv_obj_set_size(btn_row, 368, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(btn_row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_top(btn_row, 8, 0);
        lv_obj_set_style_pad_bottom(btn_row, 8, 0);
        lv_obj_set_style_border_side(btn_row, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_color(btn_row, lv_color_hex(CLR_BORDER), 0);
        lv_obj_set_style_border_width(btn_row, 1, 0);
        uiw_label(btn_row, "状态设置", CLR_TEXT_DIM);

        s_arm_btn = lv_button_create(btn_row);
        if (s_arm_btn) {
            lv_obj_set_size(s_arm_btn, 90, 32);
            lv_obj_set_style_bg_opa(s_arm_btn, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_color(s_arm_btn, lv_color_hex(CLR_CYAN), 0);
            lv_obj_set_style_border_width(s_arm_btn, 1, 0);
            lv_obj_set_style_radius(s_arm_btn, 0, 0);
            lv_obj_set_style_pad_all(s_arm_btn, 0, 0);
            lv_obj_clear_flag(s_arm_btn, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_t *bl = uiw_label(s_arm_btn, "布防", CLR_CYAN);
            if (bl) lv_obj_center(bl);   /* 字体居中 */
            lv_obj_add_event_cb(s_arm_btn, _on_arm_toggle, LV_EVENT_CLICKED, NULL);
        }
    }

    /* 构造详情行 */
    struct { const char *label; const char *value; uint32_t val_clr; } rows[] = {
        {"分区 ID",   z->id,     CLR_CYAN},
        {"分区",   z->name,   CLR_TEXT_HI},
        {"当前状态",   st_txt,    st_clr},
        {"检测", z->sensor, CLR_TEXT_HI},
        {"IP",    z->ip,     CLR_CYAN},
        {"系统",   z->fw,     CLR_CYAN},
        {"dBm",   z->rssi,   CLR_CYAN},
    };

    for (int r = 0; r < 7; r++) {
        lv_obj_t *row = uiw_obj(s_body);
        if (!row) continue;
        lv_obj_set_size(row, 368, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_top(row, 8, 0);
        lv_obj_set_style_pad_bottom(row, 8, 0);
        lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_color(row, lv_color_hex(CLR_BORDER), 0);
        lv_obj_set_style_border_width(row, 1, 0);

        uiw_label(row, rows[r].label, CLR_TEXT_DIM);
        lv_obj_t *val_lbl = uiw_label_font(row, rows[r].value, rows[r].val_clr,
                                           &lv_font_SHSC_16);
        /* 保存"当前状态"值标签（状态切换后实时更新） */
        if (r == 2) s_state_lbl = val_lbl;
    }

    _refresh_state();

    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
}

int ui_detail_visible(void)
{
    return s_overlay && !lv_obj_has_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
}

void ui_detail_build(void)
{
    s_overlay = uiw_modal_create(lv_screen_active(), "分区", 440);
    if (!s_overlay) return;

    /* 内容超出屏幕时滚动，row 定宽 368 需弹窗宽 ≥ 368+32(padding) */
    lv_obj_t *box = lv_obj_get_child(s_overlay, 0);
    if (box) {
        lv_obj_t *dbody = lv_obj_get_child(box, 1);
        if (dbody) lv_obj_set_style_max_height(dbody, 440, 0);

        /* 标题标签：box 第一个 child(head) 的第一个 child(label) */
        lv_obj_t *head = lv_obj_get_child(box, 0);
        if (head) s_title_lbl = lv_obj_get_child(head, 0);
        /* body 是 box 的第二个 child (index 1) */
        s_body = lv_obj_get_child(box, 1);
    }
}
