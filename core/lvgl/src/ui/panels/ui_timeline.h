/**
 * ui_timeline.h — 事件时间轴面板（右侧）
 *
 * 从 ui_home.c 抽出：标题栏（计数/清空）+ 可滚动事件列表 + 动态插入。
 * 事件标题在模块内保存稳定副本，点击“查看”通过回调把标题交给主页
 * （用于打开相册弹窗），避免悬空指针。
 */
#ifndef UI_TIMELINE_H
#define UI_TIMELINE_H

#include <stdint.h>
#include "lvgl/lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 点击某条事件的“查看”按钮（title 由模块持有，回调内可直接使用） */
typedef void (*ui_timeline_view_cb_t)(const char *title);

/** 设置“查看”回调（在 build 之前或之后调用均可） */
void ui_timeline_set_view_cb(ui_timeline_view_cb_t cb);

/** 构建时间轴（width 为右列宽度，height 含 32px 标题栏） */
void ui_timeline_build(lv_obj_t *parent, int width, int height);

/** 插入一条事件（置顶；超过 25 条丢弃最旧） */
void ui_timeline_add(const char *title, const char *loc,
                     uint32_t dot_clr, const char *level);

/** 清空事件列表 */
void ui_timeline_clear(void);

/** 时间轴是否已构建（未构建时写事件只跳过，避免启动早期噪音） */
int ui_timeline_ready(void);

#ifdef __cplusplus
}
#endif

#endif /* UI_TIMELINE_H */
