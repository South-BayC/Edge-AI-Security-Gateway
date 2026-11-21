/**
 * ui_status_bar.c — 顶部状态栏实现
 */
#include "ui_status_bar.h"
#include "../ui_widgets.h"
#include "../ui_theme.h"
#include "../../domain/zone_state.h"

#include <stdio.h>
#include <time.h>

static lv_obj_t *s_clock_lbl = NULL;
static lv_obj_t *s_arm_lbl = NULL;
static lv_obj_t *s_arm_dot = NULL;
static lv_obj_t *s_net_dot = NULL;
static lv_obj_t *s_net_lbl = NULL;

void ui_status_bar_build(lv_obj_t *p)
{
    lv_obj_t *bar = uiw_obj(p);
    if (!bar) return;
    lv_obj_set_size(bar, UI_SCR_W, UI_STATUS_H);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(CLR_CARD), 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(bar, lv_color_hex(CLR_BORDER), 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_left(bar, 16, 0);
    lv_obj_set_style_pad_right(bar, 16, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    /* 左: 圆点装饰 + 标题
     * 注: 原先此处用 LV_SYMBOL_HOME 图标，但 uiw_label 默认中文字库 SHSC_16
     * 无 FontAwesome 字形，会渲染成半个字符乱码，故改为纯色圆点装饰 */
    lv_obj_t *gl = uiw_obj(bar);
    if (gl) {
        /* 自适应内容宽度，避免被布局压缩裁字 */
        lv_obj_set_size(gl, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(gl, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(gl, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_gap(gl, 10, 0);
        lv_obj_clear_flag(gl, LV_OBJ_FLAG_SCROLLABLE);
        uiw_dot(gl, CLR_CYAN, 8);
        uiw_label_font(gl, "智慧安防网关", CLR_TEXT_HI, &lv_font_SHSC_16);
        uiw_label_font(gl, " · ", CLR_TEXT_DIM, &lv_font_SHSC_16);
        uiw_label_font(gl, "边缘在线", CLR_TEXT_DIM, &lv_font_SHSC_16);
    }

    /* 中: 时钟 */
    s_clock_lbl = uiw_label(bar, "2026-08-22 14:32:00", CLR_TEXT_LO);
    if (s_clock_lbl)
        lv_obj_set_style_text_font(s_clock_lbl, &lv_font_SHSC_16, 0);

    /* 右侧: 联网 + 布防中 + i.MX6ULL —— 用 flex 容器从右往左排列（HTML .status-right） */
    lv_obj_t *sr = uiw_obj(bar);
    if (sr) {
        lv_obj_set_size(sr, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(sr, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(sr, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_gap(sr, 12, 0);
        lv_obj_clear_flag(sr, LV_OBJ_FLAG_SCROLLABLE);

        /* 组1: 联网 */
        lv_obj_t *g1 = uiw_obj(sr);
        if (g1) {
            lv_obj_set_size(g1, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
            lv_obj_set_flex_flow(g1, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(g1, LV_FLEX_ALIGN_START,
                                  LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_gap(g1, 5, 0);
            lv_obj_clear_flag(g1, LV_OBJ_FLAG_SCROLLABLE);
            s_net_dot = uiw_dot(g1, CLR_GREEN, 8);
            s_net_lbl = uiw_label(g1, "联网", CLR_TEXT_LO);
            if (s_net_lbl) lv_obj_set_style_text_font(s_net_lbl, &lv_font_SHSC_16, 0);
        }

        /* 组2: 布防 */
        lv_obj_t *g2 = uiw_obj(sr);
        if (g2) {
            lv_obj_set_size(g2, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
            lv_obj_set_flex_flow(g2, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(g2, LV_FLEX_ALIGN_START,
                                  LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_gap(g2, 5, 0);
            lv_obj_clear_flag(g2, LV_OBJ_FLAG_SCROLLABLE);
            s_arm_dot = uiw_dot(g2, CLR_GREEN, 8);
            if (s_arm_dot) {
                lv_obj_set_style_shadow_color(s_arm_dot, lv_color_hex(CLR_GREEN), 0);
                lv_obj_set_style_shadow_width(s_arm_dot, 6, 0);
                lv_obj_set_style_shadow_opa(s_arm_dot, LV_OPA_COVER, 0);
            }
            s_arm_lbl = uiw_label(g2, "已撤防", CLR_TEXT_LO);
            if (s_arm_lbl) lv_obj_set_style_text_font(s_arm_lbl, &lv_font_SHSC_16, 0);
        }

        /* 组3: i.MX6ULL */
        lv_obj_t *cpu_lbl = uiw_label(sr, "i.MX6ULL", CLR_TEXT_DIM);
        if (cpu_lbl) lv_obj_set_style_text_font(cpu_lbl, &lv_font_SHSC_16, 0);
    }
}

void ui_status_bar_update_clock(void)
{
    if (!s_clock_lbl) return;

    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);

    char buf[32];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
             tm_now.tm_year + 1900, tm_now.tm_mon + 1, tm_now.tm_mday,
             tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec);
    lv_label_set_text(s_clock_lbl, buf);
}

void ui_status_bar_refresh_arm(void)
{
    int armed = zone_state_any_armed();
    uint32_t clr = armed ? CLR_GREEN : CLR_AMBER;
    if (s_arm_lbl)
        lv_label_set_text(s_arm_lbl, armed ? "布防中" : "已撤防");
    if (s_arm_dot) {
        lv_obj_set_style_bg_color(s_arm_dot, lv_color_hex(clr), 0);
        lv_obj_set_style_shadow_color(s_arm_dot, lv_color_hex(clr), 0);
        lv_obj_set_style_shadow_width(s_arm_dot, 6, 0);
        lv_obj_set_style_shadow_opa(s_arm_dot, LV_OPA_COVER, 0);
    }
}

void ui_status_bar_set_net(int online)
{
    lv_color_t clr = online ? lv_color_hex(CLR_GREEN) : lv_color_hex(CLR_TEXT_LO);
    const char *txt = online ? "联网" : "离线";
    if (s_net_dot) {
        lv_obj_set_style_bg_color(s_net_dot, clr, 0);
        lv_obj_set_style_shadow_color(s_net_dot, clr, 0);
    }
    if (s_net_lbl) {
        lv_label_set_text(s_net_lbl, txt);
        lv_obj_set_style_text_color(s_net_lbl, clr, 0);
    }
}
