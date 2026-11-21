/**
 * ui_timeline.c — 事件时间轴实现
 */
#include "ui_timeline.h"
#include "../ui_widgets.h"
#include "../ui_theme.h"
#include "../ui_time.h"

#include <stdio.h>
#include <string.h>

#define EVT_MAX 25
#define TITLE_MAX 40

static lv_obj_t *s_list = NULL;
static lv_obj_t *s_count_lbl = NULL;
static int s_count = 0;
static int s_width = UI_SCR_W;   /* build 时更新 */
static char s_titles[EVT_MAX][TITLE_MAX];
static ui_timeline_view_cb_t s_view_cb = NULL;

void ui_timeline_set_view_cb(ui_timeline_view_cb_t cb)
{
    s_view_cb = cb;
}

static void _update_count(void)
{
    if (!s_count_lbl) return;
    char b[8];
    snprintf(b, sizeof(b), "%d 条", s_count);
    lv_label_set_text(s_count_lbl, b);
}

static void _on_view_clicked(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    lv_obj_t *item = lv_obj_get_parent(btn);
    int idx = item ? (int)lv_obj_get_index(item) : -1;

    if (!s_view_cb) return;
    s_view_cb((idx >= 0 && idx < EVT_MAX) ? s_titles[idx] : "");
}

static void _on_clear(lv_event_t *e)
{
    (void)e;
    if (s_list) lv_obj_clean(s_list);
    s_count = 0;
    _update_count();
}

/* 事件条目（HTML .event-item：时间+圆点+内容+级别徽章+查看按钮） */
static lv_obj_t *_append_item(const char *time, const char *title, const char *loc,
                              uint32_t dot_clr, const char *level)
{
    if (!s_list) return NULL;

    lv_obj_t *item = uiw_obj(s_list);
    if (!item) return NULL;
    lv_obj_set_width(item, s_width - 2);
    lv_obj_set_height(item, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(item, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(item, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(item, 10, 0);
    lv_obj_set_style_pad_left(item, 12, 0);
    lv_obj_set_style_pad_right(item, 12, 0);
    lv_obj_set_style_pad_top(item, 8, 0);
    lv_obj_set_style_pad_bottom(item, 8, 0);
    lv_obj_set_style_border_side(item, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(item, lv_color_hex(CLR_BORDER), 0);
    lv_obj_set_style_border_width(item, 1, 0);
    lv_obj_clear_flag(item, LV_OBJ_FLAG_SCROLLABLE);

    /* 时间 */
    lv_obj_t *tl = uiw_label_font(item, time, CLR_TEXT_DIM, &lv_font_SHSC_16);
    if (tl) lv_label_set_long_mode(tl, LV_LABEL_LONG_DOT);

    /* 圆点 */
    uiw_dot(item, dot_clr, 6);

    /* 内容区 */
    lv_obj_t *body = uiw_obj(item);
    if (body) {
        lv_obj_set_flex_grow(body, 1);
        lv_obj_set_height(body, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_gap(body, 2, 0);

        lv_obj_t *title_lbl = uiw_label(body, title, CLR_TEXT_HI);
        if (title_lbl) lv_label_set_long_mode(title_lbl, LV_LABEL_LONG_DOT);
        lv_obj_t *loc_lbl = uiw_label(body, loc ? loc : "", CLR_TEXT_DIM);
        if (loc_lbl) lv_label_set_long_mode(loc_lbl, LV_LABEL_LONG_DOT);

        /* 级别徽章（high/medium/low 均显示，与 HTML getLevelBadge 一致） */
        if (level && level[0]) {
            uint32_t badge_bg, badge_tx;
            const char *badge_txt;
            if (strcmp(level, "high") == 0) {
                badge_bg = CLR_RED; badge_tx = CLR_RED;
                badge_txt = "级别：高";
            } else if (strcmp(level, "medium") == 0) {
                badge_bg = CLR_AMBER; badge_tx = CLR_AMBER;
                badge_txt = "级别：中";
            } else {
                badge_bg = CLR_CYAN; badge_tx = CLR_CYAN;
                badge_txt = "级别：L";
            }
            lv_obj_t *badge = uiw_obj(body);
            if (badge) {
                lv_obj_set_height(badge, LV_SIZE_CONTENT);
                lv_obj_set_width(badge, LV_SIZE_CONTENT);
                lv_obj_set_style_bg_color(badge, lv_color_hex(badge_bg), 0);
                lv_obj_set_style_bg_opa(badge, LV_OPA_20, 0);
                lv_obj_set_style_border_color(badge, lv_color_hex(badge_bg), 0);
                lv_obj_set_style_border_width(badge, 1, 0);
                lv_obj_set_style_pad_left(badge, 6, 0);
                lv_obj_set_style_pad_right(badge, 6, 0);
                lv_obj_set_style_pad_top(badge, 2, 0);
                lv_obj_set_style_pad_bottom(badge, 2, 0);
                lv_obj_t *badge_lbl = uiw_label(badge, badge_txt, badge_tx);
                if (badge_lbl) lv_label_set_long_mode(badge_lbl, LV_LABEL_LONG_DOT);
            }
        }
    }

    /* "查看"按钮（HTML .event-view-btn），点击弹出相册 */
    lv_obj_t *vbtn = lv_button_create(item);
    if (vbtn) {
        /* 固定宽 46×22：SHSC_16 汉字"览"最小 16px，加大按钮保证完整显示不截断 */
        lv_obj_set_size(vbtn, 46, 22);
        lv_obj_set_style_bg_opa(vbtn, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_color(vbtn, lv_color_hex(CLR_BORDER), 0);
        lv_obj_set_style_border_width(vbtn, 1, 0);
        lv_obj_set_style_radius(vbtn, 0, 0);
        lv_obj_set_style_pad_all(vbtn, 0, 0);
        lv_obj_clear_flag(vbtn, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t *vl = uiw_label(vbtn, "览", CLR_TEXT_LO);
        if (vl) lv_obj_center(vl);
        lv_obj_add_event_cb(vbtn, _on_view_clicked, LV_EVENT_CLICKED, NULL);
    }
    return item;
}

void ui_timeline_build(lv_obj_t *parent, int width, int height)
{
    if (!parent || width <= 0 || height <= 0) return;
    s_width = width;

    /* 标题栏 */
    lv_obj_t *hdr = uiw_obj(parent);
    if (hdr) {
        lv_obj_set_size(hdr, width, 32);
        lv_obj_set_style_bg_opa(hdr, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(hdr, lv_color_hex(CLR_CARD), 0);
        lv_obj_set_style_border_color(hdr, lv_color_hex(CLR_BORDER), 0);
        lv_obj_set_style_border_width(hdr, 1, 0);
        lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_SPACE_BETWEEN,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_left(hdr, 12, 0);
        lv_obj_set_style_pad_right(hdr, 12, 0);
        lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *evt_hdr_lbl = uiw_label(hdr, "事件时间轴", CLR_TEXT_LO);
        if (evt_hdr_lbl) lv_label_set_long_mode(evt_hdr_lbl, LV_LABEL_LONG_DOT);

        /* 计数徽章 */
        s_count_lbl = lv_label_create(hdr);
        if (s_count_lbl) {
            char cnt_buf[8];
            snprintf(cnt_buf, sizeof(cnt_buf), "%d 条", s_count);
            lv_label_set_text(s_count_lbl, cnt_buf);
            lv_label_set_long_mode(s_count_lbl, LV_LABEL_LONG_DOT);
            lv_obj_set_style_text_font(s_count_lbl, &lv_font_SHSC_16, 0);
            lv_obj_set_style_text_color(s_count_lbl, lv_color_hex(CLR_CYAN), 0);
            lv_obj_set_style_bg_color(s_count_lbl, lv_color_hex(CLR_CYAN), 0);
            lv_obj_set_style_bg_opa(s_count_lbl, LV_OPA_20, 0);
            lv_obj_set_style_pad_left(s_count_lbl, 6, 0);
            lv_obj_set_style_pad_right(s_count_lbl, 6, 0);
            lv_obj_set_style_pad_top(s_count_lbl, 2, 0);
            lv_obj_set_style_pad_bottom(s_count_lbl, 2, 0);
        }

        /* 清空按钮 */
        lv_obj_t *clr_btn = lv_button_create(hdr);
        if (clr_btn) {
            lv_obj_set_size(clr_btn, LV_SIZE_CONTENT, 26);
            lv_obj_set_style_bg_opa(clr_btn, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_color(clr_btn, lv_color_hex(CLR_BORDER), 0);
            lv_obj_set_style_border_width(clr_btn, 1, 0);
            lv_obj_set_style_radius(clr_btn, 0, 0);
            lv_obj_set_style_pad_left(clr_btn, 10, 0);
            lv_obj_set_style_pad_right(clr_btn, 10, 0);
            lv_obj_clear_flag(clr_btn, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_t *cl = uiw_label(clr_btn, "清空", CLR_TEXT_LO);
            if (cl) lv_obj_center(cl);
            lv_obj_add_event_cb(clr_btn, _on_clear, LV_EVENT_CLICKED, NULL);
        }
    }

    /* 列表（height - 32 = 滚动区高度） */
    s_list = uiw_obj(parent);
    if (!s_list) return;
    lv_obj_set_size(s_list, width, height - 32);
    lv_obj_set_style_bg_opa(s_list, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_list, lv_color_hex(CLR_CARD), 0);
    lv_obj_set_style_border_color(s_list, lv_color_hex(CLR_BORDER), 0);
    lv_obj_set_style_border_width(s_list, 1, 0);
    lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_top(s_list, 4, 0);
    lv_obj_set_style_pad_bottom(s_list, 4, 0);

    _update_count();
}

void ui_timeline_add(const char *title, const char *loc,
                     uint32_t dot_clr, const char *level)
{
    if (!s_list || !title) return;

    char t[8];
    ui_time_str(t, sizeof(t));

    lv_obj_t *item = _append_item(t, title, loc, dot_clr, level);
    if (!item) return;

    /* 插入到列表顶部 */
    lv_obj_move_to_index(item, 0);

    if (s_count >= EVT_MAX) {
        /* 超限：删除最旧条目，标题右移后覆盖末尾 */
        uint32_t cnt = lv_obj_get_child_count(s_list);
        if (cnt > EVT_MAX) {
            lv_obj_t *last = lv_obj_get_child(s_list, cnt - 1);
            if (last) lv_obj_delete(last);
        }
        memmove(&s_titles[1], &s_titles[0], (EVT_MAX - 1) * sizeof(s_titles[0]));
    } else {
        memmove(&s_titles[1], &s_titles[0], (size_t)s_count * sizeof(s_titles[0]));
        s_count++;
    }
    snprintf(s_titles[0], sizeof(s_titles[0]), "%s", title);
    _update_count();
}

void ui_timeline_clear(void)
{
    if (s_list) lv_obj_clean(s_list);
    s_count = 0;
    _update_count();
}

int ui_timeline_ready(void)
{
    return s_list != NULL;
}
