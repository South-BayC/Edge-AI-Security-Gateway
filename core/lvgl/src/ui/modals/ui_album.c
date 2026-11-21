/**
 * ui_album.c — 抓拍相册 + 查看器（从 ui_home.c 抽出）
 *
 * 持有抓拍缓冲（6 × 全图 + 缩略图，静态分配）、相册网格、查看器；
 * 抓拍来源：手动按钮 / 预警触发；TF 持久化由 storage_mgr 提供。
 */
#include "ui_album.h"
#include "../ui_widgets.h"
#include "../ui_theme.h"
#include "../ui_internal.h"
#include "../ui_canvas.h"
#include "../ui_time.h"
#include "../../cam_feed.h"
#include "../../detector.h"
#include "../../storage_mgr.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define SNAP_MAX 6
#define SNAP_W   CAM_DISP_W          /* 630，与预览一致 */
#define SNAP_H   CAM_DISP_H          /* 340 */
#define THUMB_W  240                 /* 缩略图尺寸 */
#define THUMB_H  135

typedef struct {
    uint16_t pix[SNAP_W * SNAP_H];     /* 全图 RGB565（查看器显示） */
    uint16_t thumb[THUMB_W * THUMB_H]; /* 缩略图 RGB565（相册 3×2） */
    char time[8];
    char zone[24];
    char level[12];
} snapshot_t;

static snapshot_t g_snaps[SNAP_MAX];
static int g_snap_count = 0;

static lv_obj_t *g_album_overlay = NULL;
static lv_obj_t *g_album_title_lbl = NULL;
static lv_obj_t *g_album_body = NULL;

static lv_obj_t *g_viewer_overlay = NULL;
static lv_obj_t *g_viewer_canvas = NULL;
static lv_obj_t *g_viewer_title_lbl = NULL;

/* 前向声明（本文件内互调） */
static void _rebuild_album_grid(void);
static void _open_viewer(int idx);
static void snapshot_capture(const char *zone, const char *level);

void ui_album_capture(int zone_idx, const char *zone, const char *level)
{
    /* 1. 环形插入到槽位 0（最新在前），超出 SNAP_MAX 覆盖最旧 */
    if (g_snap_count < SNAP_MAX) g_snap_count++;
    for (int i = g_snap_count - 1; i > 0; i--)
        g_snaps[i] = g_snaps[i - 1];

    /* 2. 拷贝当前摄像头帧（未工作则填黑，保留元数据可追溯）；
     *    成功则叠加精判检出框（3s 保持窗；告警/手动抓拍均带框，08-29） */
    if (cam_feed_copy_frame((uint8_t *)g_snaps[0].pix) == 0) {
        detector_box_t boxes[8];
        int nb = detector_get_boxes(boxes, 8);
        for (int i = 0; i < nb; i++)
            ui_canvas_draw_box((uint8_t *)g_snaps[0].pix, SNAP_W, SNAP_H,
                                       boxes[i].x, boxes[i].y,
                                       boxes[i].w, boxes[i].h, 0xFB29);
    } else {
        memset(g_snaps[0].pix, 0, sizeof(g_snaps[0].pix));
    }

    /* 3. 缩略图降采样（最近邻：630×340 → 240×135） */
    for (int ty = 0; ty < THUMB_H; ty++) {
        int sy = ty * SNAP_H / THUMB_H;
        const uint16_t *sline = g_snaps[0].pix + sy * SNAP_W;
        uint16_t *dline = g_snaps[0].thumb + ty * THUMB_W;
        for (int tx = 0; tx < THUMB_W; tx++) {
            dline[tx] = sline[tx * SNAP_W / THUMB_W];
        }
    }

    /* 4. 元数据 + TF 持久化（BMP 存档；无 TF 卡跳过） */
    ui_time_str(g_snaps[0].time, sizeof(g_snaps[0].time));
    snprintf(g_snaps[0].zone,  sizeof(g_snaps[0].zone),  "%s", zone ? zone : "手动");
    snprintf(g_snaps[0].level, sizeof(g_snaps[0].level), "%s", level ? level : "");
    {
        /* 级别串转 TF 文件名编码：high→hi medium→md low→lo */
        const char *cmp_lv = "none";
        if (strcmp(g_snaps[0].level, "high") == 0)   cmp_lv = "hi";
        else if (strcmp(g_snaps[0].level, "medium") == 0) cmp_lv = "md";
        else if (strcmp(g_snaps[0].level, "low") == 0)    cmp_lv = "lo";
        storage_snap_save(g_snaps[0].pix, SNAP_W, SNAP_H,
                          zone_idx, cmp_lv, g_snaps[0].time);
    }

    /* 5. 相册打开则重绘 */
    if (g_album_body)
        _rebuild_album_grid();
}

static void snapshot_capture(const char *zone, const char *level)
{
    /* 手动/无索引抓拍：区域名反查分区索引（供 TF 文件名编码） */
    int idx = 0;
    for (int i = 0; i < ZONE_COUNT; i++) {
        if (strcmp(zone_info[i].name, zone) == 0) { idx = i; break; }
    }
    ui_album_capture(idx, zone, level);
}

static void _album_level_style(const char *level, uint32_t *clr, const char **txt)
{
    if (level && strcmp(level, "high") == 0) {
        *clr = CLR_RED;   *txt = "高";
    } else if (level && strcmp(level, "medium") == 0) {
        *clr = CLR_AMBER; *txt = "中";
    } else if (level && strcmp(level, "low") == 0) {
        *clr = CLR_CYAN;  *txt = "L";
    } else {
        *clr = CLR_CYAN;  *txt = "";
    }
}

static void _on_thumb_click(lv_event_t *e)
{
    _open_viewer((int)(intptr_t)lv_event_get_user_data(e));
}

static void _rebuild_album_grid(void)
{
    if (!g_album_body) return;
    lv_obj_clean(g_album_body);

    const int cols = 3;
    int rows = (g_snap_count + cols - 1) / cols;
    if (rows < 1) rows = 1;

    for (int row = 0; row < rows; row++) {
        lv_obj_t *row_obj = uiw_obj(g_album_body);
        if (!row_obj) continue;
        lv_obj_set_size(row_obj, 744, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row_obj, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row_obj, LV_FLEX_ALIGN_SPACE_BETWEEN,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_gap(row_obj, 12, 0);
        lv_obj_set_style_pad_bottom(row_obj, 12, 0);

        for (int col = 0; col < cols; col++) {
            int idx = row * cols + col;
            if (idx >= g_snap_count) break;

            lv_obj_t *thumb = uiw_obj(row_obj);
            if (!thumb) continue;
            lv_obj_set_size(thumb, THUMB_W, THUMB_H);
            lv_obj_set_style_bg_color(thumb, lv_color_hex(0x000000), 0);
            lv_obj_set_style_bg_opa(thumb, LV_OPA_COVER, 0);
            lv_obj_set_style_border_color(thumb, lv_color_hex(CLR_BORDER), 0);
            lv_obj_set_style_border_width(thumb, 1, 0);
            lv_obj_clear_flag(thumb, LV_OBJ_FLAG_SCROLLABLE);

            /* 真实缩略图画布 */
            lv_obj_t *cv = lv_canvas_create(thumb);
            if (cv) {
                lv_canvas_set_buffer(cv, g_snaps[idx].thumb,
                                     THUMB_W, THUMB_H, LV_COLOR_FORMAT_RGB565);
                lv_obj_clear_flag(cv, LV_OBJ_FLAG_SCROLLABLE);
            }

            uint32_t lvl_clr;
            const char *lvl_txt;
            _album_level_style(g_snaps[idx].level, &lvl_clr, &lvl_txt);

            /* 左上角：抓拍区域 + 预警级别 */
            lv_obj_t *tag = uiw_obj(thumb);
            if (tag) {
                lv_obj_align(tag, LV_ALIGN_TOP_LEFT, 4, 4);
                lv_obj_set_size(tag, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
                lv_obj_set_style_bg_color(tag, lv_color_hex(0x000000), 0);
                lv_obj_set_style_bg_opa(tag, LV_OPA_50, 0);
                lv_obj_set_style_pad_left(tag, 4, 0);
                lv_obj_set_style_pad_right(tag, 4, 0);
                lv_obj_set_style_pad_top(tag, 1, 0);
                lv_obj_set_style_pad_bottom(tag, 1, 0);
                char tag_buf[24];
                if (lvl_txt[0])
                    snprintf(tag_buf, sizeof(tag_buf), "%s %s",
                             g_snaps[idx].zone, lvl_txt);
                else
                    snprintf(tag_buf, sizeof(tag_buf), "%s", g_snaps[idx].zone);
                uiw_label(tag, tag_buf, CLR_TEXT_HI);
            }

            /* 右下角：抓拍时间（小字，黑底半透明） */
            lv_obj_t *tl = uiw_obj(thumb);
            if (tl) {
                lv_obj_align(tl, LV_ALIGN_BOTTOM_RIGHT, -4, -3);
                lv_obj_set_size(tl, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
                lv_obj_set_style_bg_color(tl, lv_color_hex(0x000000), 0);
                lv_obj_set_style_bg_opa(tl, LV_OPA_50, 0);
                lv_obj_set_style_pad_left(tl, 4, 0);
                lv_obj_set_style_pad_right(tl, 4, 0);
                uiw_label_font(tl, g_snaps[idx].time, CLR_TEXT_HI, &lv_font_SHSC_16);
            }

            /* 点击缩略图 → 查看大图 */
            lv_obj_add_flag(thumb, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(thumb, _on_thumb_click, LV_EVENT_CLICKED,
                               (void *)(intptr_t)idx);
        }
    }
}

static void _build_album_modal(void)
{
    g_album_overlay = uiw_modal_create(lv_screen_active(),
                                        "抓拍相册", 780);
    if (!g_album_overlay) return;

    lv_obj_t *box = lv_obj_get_child(g_album_overlay, 0);
    if (!box) return;

    /* 获取标题标签（动态更新） */
    lv_obj_t *head = lv_obj_get_child(box, 0);
    if (head) g_album_title_lbl = lv_obj_get_child(head, 0);

    lv_obj_t *body = lv_obj_get_child(box, 1);
    if (!body) return;
    g_album_body = body;
    /* 内容超出屏幕时滚动，保证相册弹窗接近屏幕大小 */
    lv_obj_set_style_max_height(body, 480, 0);

    /* 相册初始为空：启动恢复函数（_snapshots_restore）从 TF 回填 */
    g_snap_count = 0;
    _rebuild_album_grid();
}

static void _snapshots_restore(void)
{
    char paths[SNAP_MAX][160];
    int n = storage_snap_list(paths, SNAP_MAX);
    if (n <= 0) return;

    for (int i = 0; i < n; i++) {
        /* 读 BMP 像素（失败跳过该文件） */
        if (storage_bmp_read(paths[i], g_snaps[i].pix, SNAP_W, SNAP_H) != 0)
            continue;

        /* 降采样缩略图 */
        for (int ty = 0; ty < THUMB_H; ty++) {
            int sy = ty * SNAP_H / THUMB_H;
            const uint16_t *sline = g_snaps[i].pix + sy * SNAP_W;
            uint16_t *dline = g_snaps[i].thumb + ty * THUMB_W;
            for (int tx = 0; tx < THUMB_W; tx++)
                dline[tx] = sline[tx * SNAP_W / THUMB_W];
        }

        /* 文件名解析元数据: snap_z<zone>_<cmp>_<HHMMSS>.bmp */
        char *fname = strrchr(paths[i], '/');
        fname = fname ? fname + 1 : paths[i];
        int z = 0;
        char cmp[8] = "none", hms[16] = "";
        if (sscanf(fname, "snap_z%d_%7[^_]_%15s", &z, cmp, hms) != 3) {
            z = 0;
            snprintf(cmp, sizeof(cmp), "none");
            snprintf(hms, sizeof(hms), "000000");
        }

        /* cmp → level */
        const char *lv = "";
        if (strcmp(cmp, "hi") == 0)      lv = "high";
        else if (strcmp(cmp, "md") == 0) lv = "medium";
        else if (strcmp(cmp, "lo") == 0) lv = "low";

        snprintf(g_snaps[i].zone, sizeof(g_snaps[i].zone), "%s",
                 (z >= 0 && z < ZONE_COUNT) ? zone_info[z].name : "手动");
        snprintf(g_snaps[i].level, sizeof(g_snaps[i].level), "%s", lv);
        if (strlen(hms) >= 4)
            snprintf(g_snaps[i].time, sizeof(g_snaps[i].time),
                     "%.2s:%.2s", hms, hms + 2);
        else
            snprintf(g_snaps[i].time, sizeof(g_snaps[i].time), "--:--");

        g_snap_count = i + 1;
    }

    _rebuild_album_grid();
}

static void _open_viewer(int idx)
{
    if (!g_viewer_overlay || !g_viewer_canvas) return;
    if (idx < 0 || idx >= g_snap_count) return;

    /* 大图画布直接绑定该照片的存储缓冲（零拷贝） */
    lv_canvas_set_buffer(g_viewer_canvas, (void *)g_snaps[idx].pix,
                         SNAP_W, SNAP_H, LV_COLOR_FORMAT_RGB565);
    if (g_viewer_title_lbl) {
        char b[48];
        snprintf(b, sizeof(b), "%s · %s", g_snaps[idx].zone, g_snaps[idx].time);
        lv_label_set_text(g_viewer_title_lbl, b);
    }
    lv_obj_invalidate(g_viewer_canvas);
    lv_obj_clear_flag(g_viewer_overlay, LV_OBJ_FLAG_HIDDEN);
}

static void _build_viewer_modal(void)
{
    g_viewer_overlay = uiw_modal_create(lv_screen_active(),
                                         "抓拍查看", 660);
    if (!g_viewer_overlay) return;

    lv_obj_t *box = lv_obj_get_child(g_viewer_overlay, 0);
    if (!box) return;

    lv_obj_t *head = lv_obj_get_child(box, 0);
    if (head) g_viewer_title_lbl = lv_obj_get_child(head, 0);

    lv_obj_t *body = lv_obj_get_child(box, 1);
    if (!body) return;

    g_viewer_canvas = lv_canvas_create(body);
    if (g_viewer_canvas) {
        /* 缓冲在 _open_viewer 时绑定到选中照片（零拷贝） */
        lv_obj_set_size(g_viewer_canvas, SNAP_W, SNAP_H);
        lv_obj_align(g_viewer_canvas, LV_ALIGN_TOP_MID, 0, 0);
        lv_obj_clear_flag(g_viewer_canvas, LV_OBJ_FLAG_SCROLLABLE);
    }
}


/* ================================================================
 * 对外接口
 * ================================================================ */
void ui_album_build(void)
{
    _build_album_modal();
    _build_viewer_modal();
}

void ui_album_restore(void)
{
    _snapshots_restore();
}

void ui_album_open(const char *title)
{
    if (!g_album_overlay) return;

    if (g_album_title_lbl) {
        char buf[64];
        snprintf(buf, sizeof(buf), "抓拍相册 · %s", title ? title : "");
        lv_label_set_text(g_album_title_lbl, buf);
    }
    lv_obj_clear_flag(g_album_overlay, LV_OBJ_FLAG_HIDDEN);
}

int ui_album_visible(void)
{
    return g_album_overlay && !lv_obj_has_flag(g_album_overlay, LV_OBJ_FLAG_HIDDEN);
}

int ui_album_viewer_visible(void)
{
    return g_viewer_overlay && !lv_obj_has_flag(g_viewer_overlay, LV_OBJ_FLAG_HIDDEN);
}

void ui_album_capture_current(const char *zone, const char *level)
{
    snapshot_capture(zone, level);
}
