/**
 * ui_detail.h — 分区详情弹窗（状态设置：布防 ⇄ 撤防）
 *
 * 从 ui_home.c 抽出。点击分区卡片打开；告警中点击先消警。
 */
#ifndef UI_DETAIL_H
#define UI_DETAIL_H

#include "lvgl/lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 创建弹窗（初始隐藏） */
void ui_detail_build(void);

/** 打开指定分区详情 */
void ui_detail_open(int zone_idx);

/** 弹窗当前是否可见 */
int ui_detail_visible(void);

/** 分区卡片点击回调（可直接注册到卡片对象） */
void ui_detail_card_click_cb(lv_event_t *e);

#ifdef __cplusplus
}
#endif

#endif /* UI_DETAIL_H */
