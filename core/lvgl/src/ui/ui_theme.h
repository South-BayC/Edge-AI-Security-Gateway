/**
 * ui_theme.h — UI 主题常量（色板 / 字体）
 *
 * 仅供 ui 层使用；domain / infra 不得包含本文件。
 * 拆分 ui_home.c 时，各子模块统一从这里取色板和字体，避免各写一份。
 */
#ifndef UI_THEME_H
#define UI_THEME_H

#include "lvgl/lvgl.h"

/* ---- 字库声明 ---- */
LV_FONT_DECLARE(lv_font_SHSC_16);
LV_FONT_DECLARE(lv_font_SHSC_20);
LV_FONT_DECLARE(lv_font_ui_cn_16);   /* 补充中文字库（检测/终端等面板用） */

#define FONT_UI (&lv_font_ui_cn_16)
#define FONT_SHSC16 (&lv_font_SHSC_16)
#define FONT_SHSC20 (&lv_font_SHSC_20)

/* ---- 色板：所有颜色以 HEX 宏定义，使用处调用 lv_color_hex() ---- */
#define CLR_BG         0x0B1220
#define CLR_CARD       0x1B2A4A
#define CLR_BORDER     0x253656
#define CLR_BORDER_LT  0x2E4268
#define CLR_TEXT_HI    0xE6EDF7
#define CLR_TEXT_LO    0x8A9AB5
#define CLR_TEXT_DIM   0x5A6A85
#define CLR_CYAN       0x00D1FF
#define CLR_GREEN      0x20C997
#define CLR_AMBER      0xFFB020
#define CLR_RED        0xFF4D4F
#define CLR_BLACK      0x000000

/* ---- 主窗口尺寸（所有面板共用） ---- */
#define UI_SCR_W     1024
#define UI_SCR_H     600
#define UI_STATUS_H  36

#endif /* UI_THEME_H */
