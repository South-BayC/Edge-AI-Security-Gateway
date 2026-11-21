/**
 * ui_sensor.c — 检测页（5 张模块卡片 + 子页：状态/校准/实时曲线/系统信息/终端）
 *
 * 从 ui_home.c 抽出。数据来源 dev_bridge_get_diag / sensor_calib / cam_feed；
 * 主页右侧的迷你传感器面板仍由 ui_home 维护。
 */
#include "ui_sensor.h"
#include "../ui_widgets.h"
#include "../ui_theme.h"
#include "../ui_internal.h"
#include "../ui_terminal.h"
#include "../../dev_bridge.h"
#include "../../sensor_calib.h"
#include "../../config_mgr.h"
#include "../../cam_feed.h"
#include "../../storage_mgr.h"
#include "../../log_svc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ---- 卡片编号 ---- */
#define SENS_AP   0
#define SENS_IMU  1
#define SENS_CAM  2
#define SENS_IO   3
#define SENS_SYS  4
#define SENS_TERM 5
#define SENS_COUNT 6

#define HIST_LEN 120
typedef struct {
    int32_t v[HIST_LEN];
    uint8_t n;
    uint8_t idx;
} hist_t;

static lv_obj_t *g_test_overlay = NULL;  /* 检测弹窗遮罩 */
static lv_obj_t *g_test_grid = NULL;     /* 卡片网格 */
static lv_obj_t *g_test_detail = NULL;   /* 子页容器 */
static int       g_test_sensor = -1;     /* 当前子页（-1=网格） */
static lv_obj_t *g_sens_val[SENS_COUNT];
static lv_obj_t *g_sens_st[SENS_COUNT];

/* 曲线数据（UI 线程独占） */
static hist_t g_h_imu_ax, g_h_imu_lin, g_h_als, g_h_ps, g_h_fps, g_h_temp;
static lv_obj_t *g_sens_chart = NULL;
static lv_chart_series_t *g_ser1 = NULL, *g_ser2 = NULL;
static lv_obj_t *g_sens_chart2 = NULL;
static lv_chart_series_t *g_ser_t = NULL;
static lv_obj_t *g_temp_lbl = NULL;
static lv_obj_t *g_ap_scale_lbl = NULL;

static lv_obj_t *g_sys_lbl[3] = { NULL, NULL, NULL };
static lv_obj_t *g_sys_log_lbl = NULL;

static lv_obj_t *g_y_lbl[5] = { NULL, NULL, NULL, NULL, NULL };
static lv_obj_t *g_x_lbl[3] = { NULL, NULL, NULL };
static lv_obj_t *g_y2_lbl[3] = { NULL, NULL, NULL };

/* 曲线量程（自适应） */
static int g_ymin = 0, g_ymax = 100;

/* IO 页 PWM 控件 */
static lv_obj_t *g_pwm_lbl = NULL;
static int g_pwm_on = 0;
static int g_pwm_freq = 1000;

/* 子页键值行 */
#define SENS_KV_MAX 8

/* AP3216C 本底校准按钮 */

/* 前向声明（本文件内互调） */
static void _sens_value(int id, char *buf, int n);
static int  _sens_online(int id);
static void _close_sensor(void);
static void _on_sens_card(lv_event_t *e);
static void _open_sensor(int id);


static void _on_test_beep_on(lv_event_t *e)
{
    (void)e;
    /* 板载蜂鸣器：每次点击翻转（BEEP 开关按钮） */
    int cur = 0;
    struct dev_diag d;
    dev_bridge_get_diag(&d);
    cur = d.beep_on;
    dev_bridge_set_beep(!cur);
}

static void _on_test_led_toggle(lv_event_t *e)
{
    (void)e;
    struct dev_diag d;
    dev_bridge_get_diag(&d);
    dev_bridge_set_led(!d.led_on);
    _show_toast(d.led_on ? "LED OFF" : "LED ON");
}

static void _on_test_pwm_toggle(lv_event_t *e)
{
    (void)e;
    g_pwm_on = !g_pwm_on;
    dev_bridge_set_pwm(g_pwm_on, g_pwm_freq);
}

static void _on_test_pwm_freq(lv_event_t *e)
{
    lv_obj_t *sl = lv_event_get_target(e);
    g_pwm_freq = (int)lv_slider_get_value(sl);
    if (g_pwm_lbl) {
        char b[8];
        snprintf(b, sizeof(b), "%dHz", g_pwm_freq);
        lv_label_set_text(g_pwm_lbl, b);
    }
}

static void _on_test_als_toggle(lv_event_t *e)
{
    (void)e;
    int en = dev_bridge_get_als_en();
    dev_bridge_set_als_en(!en);
    _show_toast(!en ? "AP3216C 采集已开启" : "AP3216C 采集已关闭");
}

static void _on_test_imu_toggle(lv_event_t *e)
{
    (void)e;
    int en = dev_bridge_get_imu_en();
    dev_bridge_set_imu_en(!en);
    _show_toast(!en ? "ICM20608 采集已开启" : "ICM20608 采集已关闭");
}

static void _on_test_motion_toggle(lv_event_t *e)
{
    (void)e;
    int en = cam_feed_get_motion_en();
    cam_feed_set_motion_en(!en);   /* 运动粗判开关（关闭后不产生命中） */
    _show_toast(!en ? "帧差粗判已开启" : "帧差粗判已关闭");
}

/* 小尺寸按钮助手 */

void ui_sensor_tick_1s(void)
{
    /* 卡片与迷你面板刷新（子页图表由 sens_timer_cb 以 5Hz 更新） */
    struct dev_diag d;
    dev_bridge_get_diag(&d);

    for (int i = 0; i < SENS_COUNT; i++) {
        if (g_sens_val[i]) {
            char b[64];
            _sens_value(i, b, sizeof(b));
            lv_label_set_text(g_sens_val[i], b);
        }
        if (g_sens_st[i]) {
            lv_obj_set_style_bg_color(g_sens_st[i],
                lv_color_hex(_sens_online(i) ? CLR_GREEN : CLR_RED), 0);
        }
    }
}

/* ================================================================
 * 传感器检测面板：卡片网格 + 子页（状态/校准/实时曲线）
 * ================================================================ */

static const char *SENS_NAME[SENS_COUNT] = { "AP3216C", "ICM20608", "摄像头", "外设", "系统", "终端" };
static const char *SENS_BADGE[SENS_COUNT] = { "AP", "IMU", "CAM", "IO", "SYS", "TTY" };
static const uint32_t SENS_CLR[SENS_COUNT] = { CLR_CYAN, 0x9B7BFF, CLR_GREEN, CLR_AMBER, CLR_TEXT_LO, 0x00FF9C };

static void _hist_push(hist_t *h, int v)
{
    h->v[h->idx] = v;
    h->idx = (uint8_t)((h->idx + 1) % HIST_LEN);
    if (h->n < HIST_LEN) h->n++;
}

static void _chart_fill(lv_obj_t *ch, lv_chart_series_t *s, const hist_t *h, int mul)
{
    if (!ch || !s) return;
    lv_chart_set_all_value(ch, s, LV_CHART_POINT_NONE);
    int start = (h->idx - h->n + HIST_LEN) % HIST_LEN;
    for (int i = 0; i < h->n; i++)
        lv_chart_set_next_value(ch, s, h->v[(start + i) % HIST_LEN] * mul);
}

static int _hist_absmax(const hist_t *h, int mul)
{
    int m = 0;
    for (int i = 0; i < h->n; i++) {
        int v = h->v[i] * mul;
        if (v < 0) v = -v;
        if (v > m) m = v;
    }
    return m;
}

/* 更新 Y 轴 5 个刻度（max→min） */
static void _chart_set_y(int min, int max)
{
    g_ymin = min;
    g_ymax = max;
    char b[24];
    for (int i = 0; i < 5; i++) {
        if (!g_y_lbl[i]) continue;
        int v = max - (max - min) * i / 4;
        snprintf(b, sizeof(b), "%d", v);
        lv_label_set_text(g_y_lbl[i], b);
    }
}

/* 自适应量程：振动/光照幅度小时也能看清起伏 */
static void _chart_update_scale(void)
{
    if (g_test_sensor == SENS_IMU && g_sens_chart) {
        int m = _hist_absmax(&g_h_imu_ax, 1);
        int l = _hist_absmax(&g_h_imu_lin, 1);
        if (l > m) m = l;
        int lim = m * 6 / 5 + 10;
        if (lim < 50) lim = 50;
        if (lim > 1700) lim = 1700;
        lv_chart_set_range(g_sens_chart, LV_CHART_AXIS_PRIMARY_Y, -lim, lim);
        _chart_set_y(-lim, lim);
    } else if (g_test_sensor == SENS_AP && g_sens_chart) {
        int m = _hist_absmax(&g_h_als, 1);
        int lim = m * 6 / 5 + 50;
        if (lim < 200) lim = 200;
        if (lim > 70000) lim = 70000;
        lv_chart_set_range(g_sens_chart, LV_CHART_AXIS_PRIMARY_Y, 0, lim);
        _chart_set_y(0, lim);
    }
}

static void _sens_value(int id, char *buf, int n)
{
    struct dev_diag d;
    dev_bridge_get_diag(&d);
    int total = -1, avail = -1;

    switch (id) {
    case SENS_AP:
        snprintf(buf, n, "光照 %d | 接近 %d", d.als_als, d.als_ps);
        break;
    case SENS_IMU:
        snprintf(buf, n, "合 %d | 线 %d", d.imu_mag_x100, d.imu_lin_x100);
        break;
    case SENS_CAM:
        snprintf(buf, n, "%d 帧 | %s", cam_feed_get_fps(),
                 cam_feed_get_motion_en() ? "侦测" : "暂停");
        break;
    case SENS_IO:
        snprintf(buf, n, "灯 %s | 蜂鸣 %s",
                 d.led_ok ? (d.led_on ? "开" : "关") : "故障",
                 d.beep_ok ? (d.beep_on ? "开" : "关") : "故障");
        break;
    case SENS_TERM:
        snprintf(buf, n, "本地 shell");
        break;
    default:
        storage_tf_capacity(&total, &avail);
        if (total > 0)
            snprintf(buf, n, "存储 %d/%dGB", avail / 1024, total / 1024);
        else
            snprintf(buf, n, "存储 故障");
        break;
    }
}

static int _sens_online(int id)
{
    struct dev_diag d;
    dev_bridge_get_diag(&d);
    switch (id) {
    case SENS_AP:  return d.als_ok;
    case SENS_IMU: return d.imu_ok;
    case SENS_CAM: return cam_feed_get_fps() > 0;
    case SENS_IO:  return d.led_ok || d.beep_ok || d.key_ok;
    default:       return 1;
    }
}

#define SENS_KV_MAX 12
static lv_obj_t *g_kv_val[SENS_KV_MAX];
static int g_kv_n = 0;

static void _sens_kv(lv_obj_t *par, const char *k, const char *v, uint32_t clr)
{
    lv_obj_t *row = uiw_obj(par);
    if (!row) return;
    lv_obj_set_size(row, 230, 24);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    uiw_label_font(row, k, CLR_TEXT_DIM, FONT_UI);
    lv_obj_t *val = uiw_label_font(row, v, clr, FONT_UI);
    if (g_kv_n < SENS_KV_MAX)
        g_kv_val[g_kv_n++] = val;
}

static void _kv_set(int idx, const char *txt, uint32_t clr)
{
    if (idx < 0 || idx >= g_kv_n || !g_kv_val[idx]) return;
    lv_label_set_text(g_kv_val[idx], txt);
    if (clr) lv_obj_set_style_text_color(g_kv_val[idx], lv_color_hex(clr), 0);
}

/* ---- 系统页：CPU / 内存 / 运行时长 / 日志尾 ---- */
static void _sys_metrics(int *cpu_pct, int *used_mb, int *total_mb, int *up_sec)
{
    *cpu_pct = 0; *used_mb = 0; *total_mb = 0; *up_sec = 0;

    FILE *fp = fopen("/proc/stat", "r");
    if (fp) {
        unsigned long long u = 0, n = 0, s = 0, i = 0, w = 0, irq = 0, sirq = 0, st = 0;
        if (fscanf(fp, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
                   &u, &n, &s, &i, &w, &irq, &sirq, &st) >= 4) {
            unsigned long long tot = u + n + s + i + w + irq + sirq + st;
            if (tot)
                *cpu_pct = (int)((tot - i - w) * 100 / tot);
        }
        fclose(fp);
    }

    fp = fopen("/proc/meminfo", "r");
    if (fp) {
        char line[128];
        long total = 0, avail = 0;
        while (fgets(line, sizeof(line), fp)) {
            if (sscanf(line, "MemTotal: %ld kB", &total) == 1) continue;
            if (sscanf(line, "MemAvailable: %ld kB", &avail) == 1) break;
        }
        fclose(fp);
        *total_mb = (int)(total / 1024);
        *used_mb = (int)((total - avail) / 1024);
    }

    fp = fopen("/proc/uptime", "r");
    if (fp) {
        double up = 0;
        if (fscanf(fp, "%lf", &up) == 1) *up_sec = (int)up;
        fclose(fp);
    }
}

static void _log_tail(char *out, int n)
{
    const char *path = "/opt/edge-gateway/logs/gateway.log";
    FILE *fp = fopen(path, "r");
    if (!fp) { snprintf(out, n, "(无日志文件)"); return; }

    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    long want = sz > 2048 ? 2048 : sz;
    fseek(fp, sz - want, SEEK_SET);

    char buf[2049];
    size_t got = fread(buf, 1, (size_t)want, fp);
    fclose(fp);
    buf[got] = 0;

    /* 只保留最后 7 行 */
    char *start = buf;
    int lines = 0;
    for (long i = (long)got - 1; i >= 0; i--) {
        if (buf[i] == '\n' && ++lines == 7) { start = buf + i + 1; break; }
    }
    snprintf(out, n, "%s", start);
}

static void _refresh_sys(void)
{
    if (!g_sys_lbl[0]) return;
    int cpu, used, total, up;
    _sys_metrics(&cpu, &used, &total, &up);
    char b[64];
    snprintf(b, sizeof(b), "%d%%", cpu);
    lv_label_set_text(g_sys_lbl[0], b);
    snprintf(b, sizeof(b), "%d/%d MB", used, total);
    lv_label_set_text(g_sys_lbl[1], b);
    snprintf(b, sizeof(b), "%dh %dm", up / 3600, (up % 3600) / 60);
    lv_label_set_text(g_sys_lbl[2], b);

    if (g_sys_log_lbl) {
        char log[1200];
        _log_tail(log, sizeof(log));
        lv_label_set_text(g_sys_log_lbl, log);
    }
}

static void _on_sens_card(lv_event_t *e);
static void _open_sensor(int id);
static void _close_sensor(void);

static void _sens_card(lv_obj_t *par, int id)
{
    lv_obj_t *c = lv_button_create(par);
    if (!c) return;
    lv_obj_set_size(c, 280, 136);
    lv_obj_set_style_bg_color(c, lv_color_hex(CLR_CARD), 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(c, lv_color_hex(CLR_BORDER), 0);
    lv_obj_set_style_border_width(c, 1, 0);
    lv_obj_set_style_radius(c, 4, 0);
    lv_obj_set_style_pad_all(c, 12, 0);
    lv_obj_set_style_pad_gap(c, 6, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *top = uiw_obj(c);
    if (top) {
        lv_obj_set_size(top, 256, 42);
        lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(top, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_gap(top, 10, 0);
        lv_obj_clear_flag(top, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *bd = uiw_obj(top);
        if (bd) {
            lv_obj_set_size(bd, 42, 42);
            lv_obj_set_style_radius(bd, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_color(bd, lv_color_hex(SENS_CLR[id]), 0);
            lv_obj_set_style_bg_opa(bd, LV_OPA_20, 0);
            lv_obj_set_style_border_color(bd, lv_color_hex(SENS_CLR[id]), 0);
            lv_obj_set_style_border_width(bd, 1, 0);
            lv_obj_t *bl = uiw_label_font(bd, SENS_BADGE[id], SENS_CLR[id],
                                          &lv_font_SHSC_16);
            if (bl) lv_obj_center(bl);
        }

        uiw_label_font(top, SENS_NAME[id], CLR_TEXT_HI, FONT_UI);

        lv_obj_t *dot = uiw_obj(top);
        if (dot) {
            lv_obj_set_size(dot, 8, 8);
            lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_color(dot, lv_color_hex(CLR_GREEN), 0);
            lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(dot, 0, 0);
            g_sens_st[id] = dot;
        }
    }

    g_sens_val[id] = uiw_label_font(c, "--", CLR_CYAN, FONT_UI);
    uiw_label_cn(c, "查看详情 >", CLR_TEXT_DIM);

    lv_obj_add_event_cb(c, _on_sens_card, LV_EVENT_CLICKED, (void *)(intptr_t)id);
}

static lv_obj_t *_make_chart(lv_obj_t *par, int ymin, int ymax,
                             uint32_t c1, uint32_t c2, int y2max)
{
    lv_obj_t *plot = uiw_obj(par);
    if (!plot) return NULL;
    lv_obj_set_size(plot, 586, 238);
    lv_obj_clear_flag(plot, LV_OBJ_FLAG_SCROLLABLE);

    /* 左侧 Y 轴 5 个刻度 */
    for (int i = 0; i < 5; i++) {
        g_y_lbl[i] = uiw_label_font(plot, "--", CLR_TEXT_DIM, FONT_UI);
        if (!g_y_lbl[i]) continue;
        lv_obj_set_width(g_y_lbl[i], 42);
        lv_obj_set_style_text_align(g_y_lbl[i], LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_pos(g_y_lbl[i], 0, i * 46);
    }

    /* 右侧副轴（接近） */
    if (y2max > 0) {
        int vals[3] = { y2max, y2max / 2, 0 };
        int ys[3] = { 0, 92, 184 };
        for (int i = 0; i < 3; i++) {
            char b[16];
            snprintf(b, sizeof(b), "%d", vals[i]);
            g_y2_lbl[i] = uiw_label_font(plot, b, CLR_TEXT_DIM, FONT_UI);
            if (!g_y2_lbl[i]) continue;
            lv_obj_set_width(g_y2_lbl[i], 38);
            lv_obj_set_pos(g_y2_lbl[i], 548, ys[i]);
        }
    }

    /* 底部时间刻度 */
    {
        static const char *xl[3] = { "-24s", "-12s", "now" };
        int xs[3] = { 46, 280, 510 };
        for (int i = 0; i < 3; i++) {
            g_x_lbl[i] = uiw_label_font(plot, xl[i], CLR_TEXT_DIM, FONT_UI);
            if (g_x_lbl[i]) lv_obj_set_pos(g_x_lbl[i], xs[i], 202);
        }
    }

    /* 轴线 */
    lv_obj_t *ax = uiw_obj(plot);
    if (ax) {
        lv_obj_set_size(ax, 1, 200);
        lv_obj_set_pos(ax, 44, 0);
        lv_obj_set_style_bg_color(ax, lv_color_hex(CLR_BORDER), 0);
        lv_obj_set_style_bg_opa(ax, LV_OPA_COVER, 0);
    }
    lv_obj_t *ay = uiw_obj(plot);
    if (ay) {
        lv_obj_set_size(ay, 500, 1);
        lv_obj_set_pos(ay, 46, 200);
        lv_obj_set_style_bg_color(ay, lv_color_hex(CLR_BORDER), 0);
        lv_obj_set_style_bg_opa(ay, LV_OPA_COVER, 0);
    }
    if (y2max > 0) {
        lv_obj_t *ay2 = uiw_obj(plot);
        if (ay2) {
            lv_obj_set_size(ay2, 1, 200);
            lv_obj_set_pos(ay2, 546, 0);
            lv_obj_set_style_bg_color(ay2, lv_color_hex(CLR_BORDER), 0);
            lv_obj_set_style_bg_opa(ay2, LV_OPA_COVER, 0);
        }
    }

    lv_obj_t *ch = lv_chart_create(plot);
    if (!ch) return NULL;
    lv_obj_set_size(ch, 500, 200);
    lv_obj_set_pos(ch, 46, 0);
    lv_chart_set_type(ch, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(ch, HIST_LEN);
    lv_chart_set_range(ch, LV_CHART_AXIS_PRIMARY_Y, ymin, ymax);
    if (y2max > 0)
        lv_chart_set_range(ch, LV_CHART_AXIS_SECONDARY_Y, 0, y2max);
    lv_chart_set_div_line_count(ch, 4, 6);
    lv_chart_set_update_mode(ch, LV_CHART_UPDATE_MODE_SHIFT);
    lv_obj_set_style_bg_color(ch, lv_color_hex(CLR_BG), 0);
    lv_obj_set_style_bg_opa(ch, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(ch, lv_color_hex(CLR_BORDER), 0);
    lv_obj_set_style_border_width(ch, 1, 0);
    lv_obj_set_style_pad_all(ch, 4, 0);
    lv_obj_set_style_line_color(ch, lv_color_hex(CLR_BORDER), LV_PART_MAIN);
    lv_obj_set_style_line_opa(ch, LV_OPA_60, LV_PART_MAIN);
    lv_obj_set_style_line_width(ch, 1, LV_PART_MAIN);
    lv_obj_set_style_line_width(ch, 1, LV_PART_ITEMS);
    lv_obj_set_style_width(ch, 4, LV_PART_INDICATOR);
    lv_obj_set_style_height(ch, 4, LV_PART_INDICATOR);

    g_ser1 = lv_chart_add_series(ch, lv_color_hex(c1), LV_CHART_AXIS_PRIMARY_Y);
    if (c2)
        g_ser2 = lv_chart_add_series(ch, lv_color_hex(c2),
                                     y2max > 0 ? LV_CHART_AXIS_SECONDARY_Y
                                                : LV_CHART_AXIS_PRIMARY_Y);
    else
        g_ser2 = NULL;

    _chart_set_y(ymin, ymax);
    return ch;
}

static void _sens_info(int id, lv_obj_t *col)
{
    struct dev_diag d;
    dev_bridge_get_diag(&d);
    char b[64];

    switch (id) {
    case SENS_AP: {
        int po = 0, io = 0, as = 1000;
        dev_bridge_get_ap_calib(&po, &io, &as);
        _sens_kv(col, "状态", d.als_ok ? "在线" : "离线",
                 d.als_ok ? CLR_GREEN : CLR_RED);
        _sens_kv(col, "采样", d.als_en ? "开" : "关",
                 d.als_en ? CLR_GREEN : CLR_TEXT_LO);
        snprintf(b, sizeof(b), "%d", d.als_als);
        _sens_kv(col, "光照", b, CLR_CYAN);
        snprintf(b, sizeof(b), "%d", d.als_ps);
        _sens_kv(col, "接近", b, CLR_CYAN);
        snprintf(b, sizeof(b), "%d", d.als_ir);
        _sens_kv(col, "红外", b, CLR_CYAN);
        snprintf(b, sizeof(b), "%d/%d", po, io);
        _sens_kv(col, "零点", b, CLR_TEXT_LO);
        snprintf(b, sizeof(b), "x%0.2f", as / 1000.0f);
        _sens_kv(col, "增益", b, CLR_TEXT_LO);
        break;
    }

    case SENS_IMU: {
        int ox, oy, oz, sx, sy, sz, to;
        int k[3], ref;
        sensor_calib_get(&ox, &oy, &oz, &sx, &sy, &sz, &to);
        sensor_calib_get_drift(k, &ref);
        _sens_kv(col, "状态", d.imu_ok ? "在线" : "离线",
                 d.imu_ok ? CLR_GREEN : CLR_RED);
        _sens_kv(col, "采样", d.imu_en ? "开" : "关",
                 d.imu_en ? CLR_GREEN : CLR_TEXT_LO);
        snprintf(b, sizeof(b), "%d/%d/%d", ox, oy, oz);
        _sens_kv(col, "零偏", b, CLR_CYAN);
        snprintf(b, sizeof(b), "%.1fC", d.imu_temp_x10 / 10.0f);
        _sens_kv(col, "温度", b, CLR_CYAN);
        snprintf(b, sizeof(b), "%d/%d/%d", d.imu_mag_x100, d.imu_lin_x100, d.imu_rms_x100);
        _sens_kv(col, "合/线/均", b, CLR_CYAN);
        snprintf(b, sizeof(b), "%d", d.imu_peak_x100);
        _sens_kv(col, "峰值", b, CLR_CYAN);
        snprintf(b, sizeof(b), "%d/%d/%d", k[0], k[1], k[2]);
        _sens_kv(col, "温漂", b, CLR_AMBER);
        snprintf(b, sizeof(b), "参考 %d", ref);
        _sens_kv(col, "基准", b, CLR_TEXT_LO);
        break;
    }

    case SENS_CAM:
        snprintf(b, sizeof(b), "%d", cam_feed_get_fps());
        _sens_kv(col, "帧率", b, CLR_GREEN);
        _sens_kv(col, "运动粗判", cam_feed_get_motion_en() ? "开" : "关",
                 cam_feed_get_motion_en() ? CLR_GREEN : CLR_TEXT_LO);
        _sens_kv(col, "对比间隔", "每 N 帧", CLR_CYAN);
        snprintf(b, sizeof(b), "%d", cam_feed_get_motion_interval());
        _sens_kv(col, "间隔 N", b, CLR_CYAN);
        snprintf(b, sizeof(b), "%d", cam_feed_get_motion_diff());
        _sens_kv(col, "灰度差", b, CLR_CYAN);
        _sens_kv(col, "画面", "640x480 -> 630x340", CLR_TEXT_LO);
        break;

    case SENS_IO:
        _sens_kv(col, "灯", d.led_ok ? (d.led_on ? "开" : "关") : "故障",
                 d.led_ok ? CLR_GREEN : CLR_RED);
        _sens_kv(col, "蜂鸣", d.beep_ok ? (d.beep_on ? "开" : "关") : "故障",
                 d.beep_ok ? CLR_GREEN : CLR_RED);
        _sens_kv(col, "调速", d.pwm_ok ? (d.pwm_on ? "开" : "关") : "故障",
                 d.pwm_ok ? CLR_AMBER : CLR_RED);
        snprintf(b, sizeof(b), "%dHz", d.pwm_freq);
        _sens_kv(col, "频率", b, CLR_AMBER);
        _sens_kv(col, "按键", d.key_ok ? (d.key_pressed ? "按下" : "松开") : "故障",
                 d.key_ok ? CLR_CYAN : CLR_RED);
        break;

    default: {
        int total = -1, avail = -1;
        storage_tf_capacity(&total, &avail);
        if (total > 0)
            snprintf(b, sizeof(b), "%d/%dGB", avail / 1024, total / 1024);
        else
            snprintf(b, sizeof(b), "故障");
        _sens_kv(col, "存储", b, total > 0 ? CLR_GREEN : CLR_RED);
        const struct app_config *cfg = config_get();
        snprintf(b, sizeof(b), "%s:%d", cfg->mqtt_broker, cfg->mqtt_port);
        _sens_kv(col, "MQTT", b, CLR_CYAN);
        _sens_kv(col, "传输", cfg->mqtt_tls ? "TLS" : "明文",
                 cfg->mqtt_tls ? CLR_GREEN : CLR_AMBER);
        _sens_kv(col, "配置", config_path(), CLR_TEXT_LO);
        break;
    }
    }
}

/* 子页数据实时刷新（与 _sens_info 的行顺序一致） */
static void _sens_detail_refresh(void)
{
    if (g_test_sensor < 0 || g_kv_n <= 0) return;

    struct dev_diag d;
    dev_bridge_get_diag(&d);
    char b[64];

    switch (g_test_sensor) {
    case SENS_AP: {
        _kv_set(0, d.als_ok ? "在线" : "离线", d.als_ok ? CLR_GREEN : CLR_RED);
        _kv_set(1, d.als_en ? "开" : "关", d.als_en ? CLR_GREEN : CLR_TEXT_LO);
        if (!d.als_en) break;   /* 关闭采集：冻结数据行 */
        snprintf(b, sizeof(b), "%d", d.als_als);   _kv_set(2, b, CLR_CYAN);
        snprintf(b, sizeof(b), "%d", d.als_ps);    _kv_set(3, b, CLR_CYAN);
        snprintf(b, sizeof(b), "%d", d.als_ir);    _kv_set(4, b, CLR_CYAN);
        int po = 0, io = 0, as = 1000;
        dev_bridge_get_ap_calib(&po, &io, &as);
        snprintf(b, sizeof(b), "%d/%d", po, io);   _kv_set(5, b, CLR_TEXT_LO);
        snprintf(b, sizeof(b), "x%0.2f", as / 1000.0f); _kv_set(6, b, CLR_TEXT_LO);
        break;
    }
    case SENS_IMU: {
        int ox, oy, oz, sx, sy, sz, to;
        int k[3], ref;
        sensor_calib_get(&ox, &oy, &oz, &sx, &sy, &sz, &to);
        sensor_calib_get_drift(k, &ref);
        _kv_set(0, d.imu_ok ? "在线" : "离线", d.imu_ok ? CLR_GREEN : CLR_RED);
        _kv_set(1, d.imu_en ? "开" : "关", d.imu_en ? CLR_GREEN : CLR_TEXT_LO);
        if (!d.imu_en) break;   /* 关闭采集：冻结数据行 */
        snprintf(b, sizeof(b), "%d/%d/%d", ox, oy, oz); _kv_set(2, b, CLR_CYAN);
        snprintf(b, sizeof(b), "%.1fC", d.imu_temp_x10 / 10.0f); _kv_set(3, b, CLR_CYAN);
        snprintf(b, sizeof(b), "%d/%d/%d", d.imu_mag_x100, d.imu_lin_x100, d.imu_rms_x100);
        _kv_set(4, b, CLR_CYAN);
        snprintf(b, sizeof(b), "%d", d.imu_peak_x100); _kv_set(5, b, CLR_CYAN);
        snprintf(b, sizeof(b), "%d/%d/%d", k[0], k[1], k[2]); _kv_set(6, b, CLR_AMBER);
        snprintf(b, sizeof(b), "参考 %d", ref); _kv_set(7, b, CLR_TEXT_LO);
        break;
    }
    case SENS_CAM:
        snprintf(b, sizeof(b), "%d", cam_feed_get_fps()); _kv_set(0, b, CLR_GREEN);
        _kv_set(1, cam_feed_get_motion_en() ? "开" : "关",
                cam_feed_get_motion_en() ? CLR_GREEN : CLR_TEXT_LO);
        snprintf(b, sizeof(b), "%d", cam_feed_get_motion_interval()); _kv_set(2, b, CLR_CYAN);
        snprintf(b, sizeof(b), "%d", cam_feed_get_motion_diff()); _kv_set(3, b, CLR_CYAN);
        break;
    case SENS_IO:
        _kv_set(0, d.led_ok ? (d.led_on ? "开" : "关") : "故障",
                d.led_ok ? CLR_GREEN : CLR_RED);
        _kv_set(1, d.beep_ok ? (d.beep_on ? "开" : "关") : "故障",
                d.beep_ok ? CLR_GREEN : CLR_RED);
        _kv_set(2, d.pwm_ok ? (d.pwm_on ? "开" : "关") : "故障",
                d.pwm_ok ? CLR_AMBER : CLR_RED);
        snprintf(b, sizeof(b), "%dHz", d.pwm_freq); _kv_set(3, b, CLR_AMBER);
        _kv_set(4, d.key_ok ? (d.key_pressed ? "按下" : "松开") : "故障",
                d.key_ok ? CLR_CYAN : CLR_RED);
        break;
    default: {
        int total = -1, avail = -1;
        storage_tf_capacity(&total, &avail);
        if (total > 0)
            snprintf(b, sizeof(b), "%d/%dGB", avail / 1024, total / 1024);
        else
            snprintf(b, sizeof(b), "故障");
        _kv_set(0, b, total > 0 ? CLR_GREEN : CLR_RED);
        const struct app_config *cfg = config_get();
        snprintf(b, sizeof(b), "%s:%d", cfg->mqtt_broker, cfg->mqtt_port);
        _kv_set(1, b, CLR_CYAN);
        _kv_set(2, cfg->mqtt_tls ? "TLS" : "明文",
                cfg->mqtt_tls ? CLR_GREEN : CLR_AMBER);
        break;
    }
    }
}

static void _reenable_btn_cb(lv_timer_t *t)
{
    lv_obj_t *b = (lv_obj_t *)lv_timer_get_user_data(t);
    if (b) lv_obj_clear_state(b, LV_STATE_DISABLED);
    _show_toast("校准完成");
}

static void _on_imu_calib(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    sensor_calib_start_zero_bias(2000);
    lv_obj_add_state(btn, LV_STATE_DISABLED);
    lv_timer_t *tm = lv_timer_create(_reenable_btn_cb, 2400, btn);
    if (tm) lv_timer_set_repeat_count(tm, 1);
    _show_toast("保持静止 2 秒…");
}

static void _on_imu_clear(lv_event_t *e)
{
    (void)e;
    sensor_calib_set(0, 0, 0, 1000, 1000, 1000, 0);
    _show_toast("已清除校准（基线归零）");
}

/* AP3216C 环境本底校准：采样 1s，把当前 PS/IR 作为新本底 */
static int g_ap_cal_n = 0;
static long g_ap_ps_sum = 0, g_ap_ir_sum = 0;
static lv_obj_t *g_ap_cal_btn = NULL;

static void _ap_cal_timer_cb(lv_timer_t *t)
{
    struct dev_diag d;
    dev_bridge_get_diag(&d);
    g_ap_ps_sum += d.als_ps;
    g_ap_ir_sum += d.als_ir;
    g_ap_cal_n++;

    if (g_ap_cal_n < 10) return;

    int old_ps = 0, old_ir = 0, scale = 1000;
    dev_bridge_get_ap_calib(&old_ps, &old_ir, &scale);
    int base_ps = old_ps + (int)(g_ap_ps_sum / 10);
    int base_ir = old_ir + (int)(g_ap_ir_sum / 10);
    dev_bridge_set_ap_calib(base_ps, base_ir, scale);

    g_ap_cal_n = 0;
    g_ap_ps_sum = g_ap_ir_sum = 0;
    if (g_ap_cal_btn)
        lv_obj_clear_state(g_ap_cal_btn, LV_STATE_DISABLED);
    _show_toast("AP3216C 本底已校准");
    lv_timer_delete(t);
}

static void _on_ap_baseline(lv_event_t *e)
{
    g_ap_cal_n = 0;
    g_ap_ps_sum = g_ap_ir_sum = 0;
    g_ap_cal_btn = lv_event_get_target(e);
    lv_obj_add_state(g_ap_cal_btn, LV_STATE_DISABLED);
    lv_timer_create(_ap_cal_timer_cb, 100, NULL);
    _show_toast("保持环境静止 1 秒…");
}

static void _on_ap_reset(lv_event_t *e)
{
    (void)e;
    dev_bridge_set_ap_calib(0, 0, 1000);
    _show_toast("AP3216C 校准已恢复默认");
}

static void _on_ap_scale_cb(lv_event_t *e)
{
    lv_obj_t *sl = lv_event_get_target(e);
    int scale = (int)lv_slider_get_value(sl);
    int po = 0, io = 0, old = 1000;
    dev_bridge_get_ap_calib(&po, &io, &old);
    dev_bridge_set_ap_calib(po, io, scale);
    if (g_ap_scale_lbl) {
        char b[16];
        snprintf(b, sizeof(b), "x%0.2f", scale / 1000.0f);
        lv_label_set_text(g_ap_scale_lbl, b);
    }
}

/* 温度零点校正：滑条 -50..50 → ±5.0℃，写入 temp_off */
static void _on_temp_off_cb(lv_event_t *e)
{
    lv_obj_t *sl = lv_event_get_target(e);
    int v = (int)lv_slider_get_value(sl);
    sensor_calib_set_temp_off(v * 33);
    if (g_temp_lbl) {
        char b[16];
        snprintf(b, sizeof(b), "%+.1fC", v / 10.0f);
        lv_label_set_text(g_temp_lbl, b);
    }
}

static void _on_back_cb(lv_event_t *e)
{
    (void)e;
    _close_sensor();
}

static void _open_sensor(int id)
{
    if (!g_test_detail || !g_test_grid || id < 0 || id >= SENS_COUNT) return;
    g_test_sensor = id;
    ui_terminal_close();   /* 切换子页时终止终端并清空对象指针 */
    g_sens_chart = NULL;
    g_sens_chart2 = NULL;
    g_ser1 = g_ser2 = NULL;
    g_ser_t = NULL;
    g_kv_n = 0;
    for (int i = 0; i < SENS_KV_MAX; i++) g_kv_val[i] = NULL;
    for (int i = 0; i < 3; i++) g_y2_lbl[i] = NULL;
    g_temp_lbl = NULL;
    g_ap_scale_lbl = NULL;
    g_sys_log_lbl = NULL;
    for (int i = 0; i < 5; i++) g_y_lbl[i] = NULL;
    for (int i = 0; i < 3; i++) g_x_lbl[i] = NULL;
    for (int i = 0; i < 3; i++) g_sys_lbl[i] = NULL;
    lv_obj_clean(g_test_detail);
    lv_obj_add_flag(g_test_grid, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(g_test_detail, LV_OBJ_FLAG_HIDDEN);

    /* 标题行：返回 + 名称 */
    lv_obj_t *hdr = uiw_obj(g_test_detail);
    if (hdr) {
        lv_obj_set_size(hdr, 860, 36);
        lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_gap(hdr, 14, 0);
        lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *back = lv_button_create(hdr);
        if (back) {
            lv_obj_set_size(back, 84, 32);
            lv_obj_set_style_bg_opa(back, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_color(back, lv_color_hex(CLR_BORDER), 0);
            lv_obj_set_style_border_width(back, 1, 0);
            lv_obj_set_style_radius(back, 4, 0);
            lv_obj_t *bl = uiw_label_cn(back, "< 返回", CLR_TEXT_LO);
            if (bl) lv_obj_center(bl);
            lv_obj_add_event_cb(back, _on_back_cb, LV_EVENT_CLICKED, NULL);
        }
        uiw_label_font(hdr, SENS_NAME[id], CLR_TEXT_HI, FONT_UI);
    }

    /* 终端：独立布局（输出 + 命令行 + 软键盘），不走左右分栏 */
    if (id == SENS_TERM) {
        ui_terminal_build(g_test_detail);
        return;
    }

    /* 内容行：左信息/操作 + 右曲线 */
    lv_obj_t *row = uiw_obj(g_test_detail);
    if (row) {
        lv_obj_set_size(row, 860, 380);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
        lv_obj_set_style_pad_gap(row, 12, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *left = uiw_obj(row);
        if (left) {
            lv_obj_set_size(left, 256, 380);
            lv_obj_set_flex_flow(left, LV_FLEX_FLOW_COLUMN);
            lv_obj_set_style_pad_all(left, 10, 0);
            lv_obj_set_style_pad_gap(left, 4, 0);
            lv_obj_set_style_bg_color(left, lv_color_hex(CLR_CARD), 0);
            lv_obj_set_style_bg_opa(left, LV_OPA_COVER, 0);
            lv_obj_set_style_border_color(left, lv_color_hex(CLR_BORDER), 0);
            lv_obj_set_style_border_width(left, 1, 0);
            lv_obj_add_flag(left, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_scroll_dir(left, LV_DIR_VER);
            lv_obj_set_scrollbar_mode(left, LV_SCROLLBAR_MODE_AUTO);

            _sens_info(id, left);

            if (id == SENS_IMU) {
                uiw_mini_btn(left, "校准 2秒", CLR_AMBER, _on_imu_calib);
                uiw_mini_btn(left, "清除", CLR_RED, _on_imu_clear);
                uiw_mini_btn(left, "采样", CLR_CYAN, _on_test_imu_toggle);

                /* 温度零点校正滑条（±5℃，写入 temp_off） */
                {
                    int ox, oy, oz, sx, sy, sz, to;
                    sensor_calib_get(&ox, &oy, &oz, &sx, &sy, &sz, &to);
                    lv_obj_t *trow = uiw_obj(left);
                    if (trow) {
                        lv_obj_set_size(trow, 230, 26);
                        lv_obj_set_flex_flow(trow, LV_FLEX_FLOW_ROW);
                        lv_obj_set_flex_align(trow, LV_FLEX_ALIGN_SPACE_BETWEEN,
                                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
                        lv_obj_clear_flag(trow, LV_OBJ_FLAG_SCROLLABLE);
                        uiw_label_font(trow, "温度", CLR_TEXT_DIM, FONT_UI);
                        char tb[16];
                        snprintf(tb, sizeof(tb), "%+.1fC", to / 326.8f);
                        g_temp_lbl = uiw_label_font(trow, tb, CLR_AMBER, &lv_font_SHSC_16);
                    }
                    lv_obj_t *tsl = lv_slider_create(left);
                    if (tsl) {
                        lv_obj_set_width(tsl, 224);
                        lv_obj_set_height(tsl, 16);
                        lv_slider_set_range(tsl, -50, 50);
                        lv_slider_set_value(tsl, (int)(to / 33.0f), LV_ANIM_OFF);
                        lv_obj_set_style_bg_color(tsl, lv_color_hex(CLR_BORDER), LV_PART_MAIN);
                        lv_obj_set_style_bg_opa(tsl, LV_OPA_COVER, LV_PART_MAIN);
                        lv_obj_set_style_bg_color(tsl, lv_color_hex(CLR_AMBER), LV_PART_INDICATOR);
                        lv_obj_set_style_bg_opa(tsl, LV_OPA_COVER, LV_PART_INDICATOR);
                        lv_obj_set_style_bg_color(tsl, lv_color_hex(CLR_AMBER), LV_PART_KNOB);
                        lv_obj_set_style_bg_opa(tsl, LV_OPA_COVER, LV_PART_KNOB);
                        lv_obj_set_style_radius(tsl, LV_RADIUS_CIRCLE, LV_PART_KNOB);
                        lv_obj_add_event_cb(tsl, _on_temp_off_cb, LV_EVENT_VALUE_CHANGED, NULL);
                    }
                }
            } else if (id == SENS_AP) {
                uiw_mini_btn(left, "本底校准", CLR_AMBER, _on_ap_baseline);
                uiw_mini_btn(left, "恢复默认", CLR_RED, _on_ap_reset);
                uiw_mini_btn(left, "采样", CLR_CYAN, _on_test_als_toggle);

                /* ALS 比例（500~2000 = 0.5x~2.0x） */
                {
                    int po = 0, io = 0, sc = 1000;
                    dev_bridge_get_ap_calib(&po, &io, &sc);
                    lv_obj_t *arow = uiw_obj(left);
                    if (arow) {
                        lv_obj_set_size(arow, 230, 26);
                        lv_obj_set_flex_flow(arow, LV_FLEX_FLOW_ROW);
                        lv_obj_set_flex_align(arow, LV_FLEX_ALIGN_SPACE_BETWEEN,
                                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
                        lv_obj_clear_flag(arow, LV_OBJ_FLAG_SCROLLABLE);
                        uiw_label_font(arow, "增益", CLR_TEXT_DIM, FONT_UI);
                        char sb[16];
                        snprintf(sb, sizeof(sb), "x%0.2f", sc / 1000.0f);
                        g_ap_scale_lbl = uiw_label_font(arow, sb, CLR_AMBER,
                                                       &lv_font_SHSC_16);
                    }
                    lv_obj_t *asl = lv_slider_create(left);
                    if (asl) {
                        lv_obj_set_width(asl, 224);
                        lv_obj_set_height(asl, 16);
                        lv_slider_set_range(asl, 500, 2000);
                        lv_slider_set_value(asl, sc, LV_ANIM_OFF);
                        lv_obj_set_style_bg_color(asl, lv_color_hex(CLR_BORDER), LV_PART_MAIN);
                        lv_obj_set_style_bg_opa(asl, LV_OPA_COVER, LV_PART_MAIN);
                        lv_obj_set_style_bg_color(asl, lv_color_hex(CLR_AMBER), LV_PART_INDICATOR);
                        lv_obj_set_style_bg_opa(asl, LV_OPA_COVER, LV_PART_INDICATOR);
                        lv_obj_set_style_bg_color(asl, lv_color_hex(CLR_AMBER), LV_PART_KNOB);
                        lv_obj_set_style_bg_opa(asl, LV_OPA_COVER, LV_PART_KNOB);
                        lv_obj_set_style_radius(asl, LV_RADIUS_CIRCLE, LV_PART_KNOB);
                        lv_obj_add_event_cb(asl, _on_ap_scale_cb, LV_EVENT_VALUE_CHANGED, NULL);
                    }
                }
            } else if (id == SENS_CAM) {
                uiw_mini_btn(left, "运动粗判", CLR_GREEN, _on_test_motion_toggle);
                uiw_label_font(left, "帧差粗判开关", CLR_TEXT_DIM, FONT_UI);
            }
        }

        lv_obj_t *right = uiw_obj(row);
        if (right) {
            lv_obj_set_size(right, 590, 380);
            lv_obj_set_flex_flow(right, LV_FLEX_FLOW_COLUMN);
            lv_obj_set_style_pad_gap(right, 6, 0);
            lv_obj_clear_flag(right, LV_OBJ_FLAG_SCROLLABLE);

            if (id == SENS_AP) {
                g_sens_chart = _make_chart(right, 0, 70000, CLR_CYAN, CLR_AMBER, 1023);
                _chart_fill(g_sens_chart, g_ser1, &g_h_als, 1);
                _chart_fill(g_sens_chart, g_ser2, &g_h_ps, 1);   /* PS 走右轴 0~1023 */
                _chart_update_scale();
                uiw_label_font(right, "光照(左) / 接近(右)", CLR_TEXT_DIM, FONT_UI);
            } else if (id == SENS_IMU) {
                g_sens_chart = _make_chart(right, -400, 400, CLR_CYAN, 0x9B7BFF, 0);
                _chart_fill(g_sens_chart, g_ser1, &g_h_imu_ax, 1);
                _chart_fill(g_sens_chart, g_ser2, &g_h_imu_lin, 1);
                _chart_update_scale();
                uiw_label_font(right, "加速度 / 线性", CLR_TEXT_DIM, FONT_UI);

                /* 温度子图（0.1℃，3 个 Y 刻度） */
                lv_obj_t *tp = uiw_obj(right);
                if (tp) {
                    lv_obj_set_size(tp, 580, 110);
                    lv_obj_clear_flag(tp, LV_OBJ_FLAG_SCROLLABLE);

                    static const char *tl[3] = { "45C", "30C", "15C" };
                    int ty[3] = { 0, 36, 72 };
                    for (int i = 0; i < 3; i++) {
                        lv_obj_t *l = uiw_label_font(tp, tl[i], CLR_TEXT_DIM, &lv_font_SHSC_16);
                        if (!l) continue;
                        lv_obj_set_width(l, 42);
                        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_RIGHT, 0);
                        lv_obj_set_pos(l, 0, ty[i]);
                    }
                    static const char *xl[3] = { "-24s", "-12s", "now" };
                    int xs[3] = { 46, 286, 538 };
                    for (int i = 0; i < 3; i++) {
                        lv_obj_t *l = uiw_label_font(tp, xl[i], CLR_TEXT_DIM, &lv_font_SHSC_16);
                        if (l) lv_obj_set_pos(l, xs[i], 84);
                    }
                    lv_obj_t *tax = uiw_obj(tp);
                    if (tax) { lv_obj_set_size(tax, 1, 80); lv_obj_set_pos(tax, 44, 0);
                               lv_obj_set_style_bg_color(tax, lv_color_hex(CLR_BORDER), 0);
                               lv_obj_set_style_bg_opa(tax, LV_OPA_COVER, 0); }
                    lv_obj_t *tay = uiw_obj(tp);
                    if (tay) { lv_obj_set_size(tay, 528, 1); lv_obj_set_pos(tay, 46, 80);
                               lv_obj_set_style_bg_color(tay, lv_color_hex(CLR_BORDER), 0);
                               lv_obj_set_style_bg_opa(tay, LV_OPA_COVER, 0); }

                    g_sens_chart2 = lv_chart_create(tp);
                    if (g_sens_chart2) {
                        lv_obj_set_size(g_sens_chart2, 528, 80);
                        lv_obj_set_pos(g_sens_chart2, 46, 0);
                        lv_chart_set_type(g_sens_chart2, LV_CHART_TYPE_LINE);
                        lv_chart_set_point_count(g_sens_chart2, HIST_LEN);
                        lv_chart_set_range(g_sens_chart2, LV_CHART_AXIS_PRIMARY_Y, 150, 450);
                        lv_chart_set_div_line_count(g_sens_chart2, 2, 6);
                        lv_chart_set_update_mode(g_sens_chart2, LV_CHART_UPDATE_MODE_SHIFT);
                        lv_obj_set_style_bg_color(g_sens_chart2, lv_color_hex(CLR_BG), 0);
                        lv_obj_set_style_bg_opa(g_sens_chart2, LV_OPA_COVER, 0);
                        lv_obj_set_style_border_color(g_sens_chart2, lv_color_hex(CLR_BORDER), 0);
                        lv_obj_set_style_border_width(g_sens_chart2, 1, 0);
                        lv_obj_set_style_pad_all(g_sens_chart2, 4, 0);
                        lv_obj_set_style_line_color(g_sens_chart2, lv_color_hex(CLR_BORDER), LV_PART_MAIN);
                        lv_obj_set_style_line_opa(g_sens_chart2, LV_OPA_60, LV_PART_MAIN);
                        lv_obj_set_style_line_width(g_sens_chart2, 1, LV_PART_MAIN);
                        lv_obj_set_style_line_width(g_sens_chart2, 1, LV_PART_ITEMS);
                        lv_obj_set_style_width(g_sens_chart2, 3, LV_PART_INDICATOR);
                        lv_obj_set_style_height(g_sens_chart2, 3, LV_PART_INDICATOR);
                        g_ser_t = lv_chart_add_series(g_sens_chart2, lv_color_hex(CLR_AMBER),
                                                      LV_CHART_AXIS_PRIMARY_Y);
                        _chart_fill(g_sens_chart2, g_ser_t, &g_h_temp, 1);
                    }
                }
            } else if (id == SENS_CAM) {
                g_sens_chart = _make_chart(right, 0, 35, CLR_GREEN, 0, 0);
                _chart_fill(g_sens_chart, g_ser1, &g_h_fps, 1);
                uiw_label_font(right, "帧率", CLR_TEXT_DIM, FONT_UI);
                uiw_label_font(right, "帧差粗判：画面变化超阈值即计入告警判定",
                               CLR_TEXT_DIM, FONT_UI);
            } else if (id == SENS_IO) {
                lv_obj_t *brow = uiw_obj(right);
                if (brow) {
                    lv_obj_set_size(brow, 560, LV_SIZE_CONTENT);
                    lv_obj_set_flex_flow(brow, LV_FLEX_FLOW_ROW);
                    lv_obj_set_style_pad_gap(brow, 10, 0);
                    lv_obj_clear_flag(brow, LV_OBJ_FLAG_SCROLLABLE);
                    uiw_mini_btn(brow, "灯", CLR_CYAN, _on_test_led_toggle);
                    uiw_mini_btn(brow, "蜂鸣", CLR_GREEN, _on_test_beep_on);
                    uiw_mini_btn(brow, "调速", CLR_AMBER, _on_test_pwm_toggle);
                }

                lv_obj_t *frow = uiw_obj(right);
                if (frow) {
                    lv_obj_set_size(frow, 560, 28);
                    lv_obj_set_flex_flow(frow, LV_FLEX_FLOW_ROW);
                    lv_obj_set_flex_align(frow, LV_FLEX_ALIGN_SPACE_BETWEEN,
                                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
                    lv_obj_clear_flag(frow, LV_OBJ_FLAG_SCROLLABLE);
                    uiw_label_font(frow, "调速频率", CLR_TEXT_LO, FONT_UI);
                    g_pwm_lbl = uiw_label_font(frow, "1000Hz", CLR_AMBER,
                                               &lv_font_SHSC_16);
                }
                lv_obj_t *sl = lv_slider_create(right);
                if (sl) {
                    lv_obj_set_width(sl, 560);
                    lv_obj_set_height(sl, 20);
                    lv_slider_set_range(sl, 100, 5000);
                    lv_slider_set_value(sl, g_pwm_freq, LV_ANIM_OFF);
                    lv_obj_set_style_bg_color(sl, lv_color_hex(CLR_BORDER), LV_PART_MAIN);
                    lv_obj_set_style_bg_opa(sl, LV_OPA_COVER, LV_PART_MAIN);
                    lv_obj_set_style_bg_color(sl, lv_color_hex(CLR_AMBER), LV_PART_INDICATOR);
                    lv_obj_set_style_bg_opa(sl, LV_OPA_COVER, LV_PART_INDICATOR);
                    lv_obj_set_style_bg_color(sl, lv_color_hex(CLR_AMBER), LV_PART_KNOB);
                    lv_obj_set_style_bg_opa(sl, LV_OPA_COVER, LV_PART_KNOB);
                    lv_obj_set_style_radius(sl, LV_RADIUS_CIRCLE, LV_PART_KNOB);
                    lv_obj_add_event_cb(sl, _on_test_pwm_freq, LV_EVENT_VALUE_CHANGED, NULL);
                }
            } else {
                /* 系统页：CPU / 内存 / 运行时长 + 日志尾 */
                static const char *sk[3] = { "CPU", "内存", "运行" };
                for (int i = 0; i < 3; i++) {
                    lv_obj_t *r = uiw_obj(right);
                    if (!r) continue;
                    lv_obj_set_size(r, 560, 26);
                    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
                    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_SPACE_BETWEEN,
                                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
                    lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
                    uiw_label_font(r, sk[i], CLR_TEXT_DIM, FONT_UI);
                    g_sys_lbl[i] = uiw_label_font(r, "--", CLR_CYAN, &lv_font_SHSC_16);
                }

                lv_obj_t *lb = uiw_obj(right);
                if (lb) {
                    lv_obj_set_size(lb, 570, 240);
                    lv_obj_set_style_pad_all(lb, 8, 0);
                    lv_obj_set_style_bg_color(lb, lv_color_hex(CLR_BG), 0);
                    lv_obj_set_style_bg_opa(lb, LV_OPA_COVER, 0);
                    lv_obj_set_style_border_color(lb, lv_color_hex(CLR_BORDER), 0);
                    lv_obj_set_style_border_width(lb, 1, 0);
                    lv_obj_set_scrollbar_mode(lb, LV_SCROLLBAR_MODE_AUTO);
                    lv_obj_set_scroll_dir(lb, LV_DIR_VER);
                    g_sys_log_lbl = lv_label_create(lb);
                    if (g_sys_log_lbl) {
                        lv_obj_set_width(g_sys_log_lbl, 548);
                        lv_label_set_long_mode(g_sys_log_lbl, LV_LABEL_LONG_WRAP);
                        lv_label_set_text(g_sys_log_lbl, "(读取中)");
                        lv_obj_set_style_text_font(g_sys_log_lbl, &lv_font_SHSC_16, 0);
                        lv_obj_set_style_text_color(g_sys_log_lbl, lv_color_hex(CLR_TEXT_LO), 0);
                    }
                }
                _refresh_sys();
            }
        }
    }
}

static void _on_sens_card(lv_event_t *e)
{
    _open_sensor((int)(intptr_t)lv_event_get_user_data(e));
}

static void _close_sensor(void)
{
    ui_terminal_close();
    g_test_sensor = -1;
    g_sens_chart = NULL;
    g_ser1 = g_ser2 = NULL;
    if (g_test_detail) {
        lv_obj_clean(g_test_detail);
        lv_obj_add_flag(g_test_detail, LV_OBJ_FLAG_HIDDEN);
    }
    if (g_test_grid)
        lv_obj_clear_flag(g_test_grid, LV_OBJ_FLAG_HIDDEN);
}

/* 采样 + 曲线/卡片刷新（100ms；曲线 5Hz、卡片 1Hz） */
void ui_sensor_tick_10hz(void)
{
    static int tick = 0;
    struct dev_diag d;
    dev_bridge_get_diag(&d);

    if (d.imu_en) {
        _hist_push(&g_h_imu_ax, d.imu_ax * 100 / 2048);
        _hist_push(&g_h_imu_lin, d.imu_lin_x100);
        _hist_push(&g_h_temp, d.imu_temp_x10);
    }
    if (d.als_en) {
        _hist_push(&g_h_als, d.als_als);
        _hist_push(&g_h_ps, d.als_ps);
    }
    if ((tick % 10) == 0)
        _hist_push(&g_h_fps, cam_feed_get_fps());

    if (g_test_sensor >= 0 && g_sens_chart && (tick % 2) == 0) {
        if (g_test_sensor == SENS_IMU && d.imu_en) {
            lv_chart_set_next_value(g_sens_chart, g_ser1, d.imu_ax * 100 / 2048);
            if (g_ser2)
                lv_chart_set_next_value(g_sens_chart, g_ser2, d.imu_lin_x100);
            if (g_sens_chart2 && g_ser_t)
                lv_chart_set_next_value(g_sens_chart2, g_ser_t, d.imu_temp_x10);
        } else if (g_test_sensor == SENS_AP && d.als_en) {
            lv_chart_set_next_value(g_sens_chart, g_ser1, d.als_als);
            if (g_ser2)
                lv_chart_set_next_value(g_sens_chart, g_ser2, d.als_ps);
        } else if (g_test_sensor == SENS_CAM) {
            lv_chart_set_next_value(g_sens_chart, g_ser1, cam_feed_get_fps());
        }
    }

    if ((tick % 2) == 0 && g_test_sensor >= 0)
        _sens_detail_refresh();   /* 子页数值 5Hz 实时联动 */

    if ((tick % 10) == 0) {
        ui_sensor_tick_1s();
        if (g_test_sensor == SENS_SYS)
            _refresh_sys();
        if (g_test_sensor == SENS_AP || g_test_sensor == SENS_IMU)
            _chart_update_scale();
    }

    tick++;
}

static void _build_test_modal(void)
{
    g_test_overlay = uiw_modal_create(lv_screen_active(),
                                       "状态检测", 920);
    if (!g_test_overlay) return;

    lv_obj_t *box = lv_obj_get_child(g_test_overlay, 0);
    if (!box) return;
    lv_obj_t *body = lv_obj_get_child(box, 1);
    if (!body) return;
    lv_obj_set_style_max_height(body, 480, 0);

    /* 卡片网格 */
    g_test_grid = uiw_obj(body);
    if (g_test_grid) {
        lv_obj_set_size(g_test_grid, 880, 460);
        lv_obj_set_flex_flow(g_test_grid, LV_FLEX_FLOW_ROW_WRAP);
        lv_obj_set_flex_align(g_test_grid, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
        lv_obj_set_style_pad_gap(g_test_grid, 12, 0);
        lv_obj_clear_flag(g_test_grid, LV_OBJ_FLAG_SCROLLABLE);
        for (int i = 0; i < SENS_COUNT; i++)
            _sens_card(g_test_grid, i);
    }

    /* 子页容器（默认隐藏） */
    g_test_detail = uiw_obj(body);
    if (g_test_detail) {
        lv_obj_set_size(g_test_detail, 880, 460);
        lv_obj_set_flex_flow(g_test_detail, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_gap(g_test_detail, 8, 0);
        lv_obj_clear_flag(g_test_detail, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(g_test_detail, LV_OBJ_FLAG_HIDDEN);
    }

    ui_sensor_tick_1s();
}

/* ================================================================
 * 对外接口
 * ================================================================ */
void ui_sensor_build(void)
{
    _build_test_modal();
}

void ui_sensor_open(void)
{
    if (!g_test_overlay) return;
    _close_sensor();          /* 每次打开回到卡片网格 */
    ui_sensor_tick_1s();      /* 打开瞬间刷新一次 */
    lv_obj_clear_flag(g_test_overlay, LV_OBJ_FLAG_HIDDEN);
}

int ui_sensor_visible(void)
{
    return g_test_overlay && !lv_obj_has_flag(g_test_overlay, LV_OBJ_FLAG_HIDDEN);
}
