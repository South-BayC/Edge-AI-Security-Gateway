/**
 * ui_canvas.h — LVGL canvas 像素绘制小工具
 */
#ifndef UI_CANVAS_H
#define UI_CANVAS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 在 RGB565 缓冲上绘制矩形框（入侵框叠加）。
 * @param buf   像素缓冲（bw*bh*2 字节）
 * @param bw    缓冲宽（像素）
 * @param bh    缓冲高（像素）
 * @param fx,fy,fw,fh 归一化坐标（0~1）
 * @param color RGB565 颜色
 */
void ui_canvas_draw_box(uint8_t *buf, int bw, int bh,
                        float fx, float fy, float fw, float fh,
                        uint16_t color);

#ifdef __cplusplus
}
#endif

#endif /* UI_CANVAS_H */
