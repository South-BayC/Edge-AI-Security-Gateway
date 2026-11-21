/**
 * ui_canvas.c — LVGL canvas 像素绘制小工具（入侵框等）
 *
 * 供摄像头预览与抓拍图片共用；只操作 RGB565 像素缓冲，不依赖具体对象。
 */
#include "ui_canvas.h"

void ui_canvas_draw_box(uint8_t *buf, int bw, int bh,
                        float fx, float fy, float fw, float fh,
                        uint16_t color)
{
    int x0 = (int)fx, y0 = (int)fy;
    int x1 = x0 + (int)fw, y1 = y0 + (int)fh;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > bw - 1) x1 = bw - 1;
    if (y1 > bh - 1) y1 = bh - 1;
    if (x1 - x0 < 4 || y1 - y0 < 4) return;   /* 过小则不画 */
    uint16_t *p = (uint16_t *)buf;
    for (int t = 0; t < 2; t++) {             /* 上/下边，2px 粗 */
        for (int x = x0; x <= x1; x++) {
            p[(y0 + t) * bw + x] = color;
            p[(y1 - t) * bw + x] = color;
        }
    }
    for (int t = 0; t < 2; t++) {             /* 左/右边 */
        for (int y = y0; y <= y1; y++) {
            p[y * bw + x0 + t] = color;
            p[y * bw + x1 - t] = color;
        }
    }
}
