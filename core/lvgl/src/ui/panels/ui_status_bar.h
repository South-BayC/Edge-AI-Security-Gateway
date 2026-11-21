/**
 * ui_status_bar.h — 顶部状态栏（标题 / 时钟 / 联网 / 布防 / CPU）
 *
 * 从 ui_home.c 抽出的独立面板：
 *   构建一次，之后由主页定时器驱动刷新（时钟 1s、联网 5s）。
 * 只做展示，状态真值来自 domain/zone_state 与调用方传入的联网结果。
 */
#ifndef UI_STATUS_BAR_H
#define UI_STATUS_BAR_H

#include "lvgl/lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 在父容器内构建状态栏（高度 UI_STATUS_H） */
void ui_status_bar_build(lv_obj_t *parent);

/** 读系统时间刷新时钟文字 */
void ui_status_bar_update_clock(void);

/** 刷新布防指示（任一分区布防=布防中） */
void ui_status_bar_refresh_arm(void);

/** 刷新联网指示（颜色 + 文字） */
void ui_status_bar_set_net(int online);

#ifdef __cplusplus
}
#endif

#endif /* UI_STATUS_BAR_H */
