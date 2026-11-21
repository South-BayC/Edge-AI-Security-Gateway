/**
 * ui_alarm.h — 告警确认弹窗（长按 1s 消警 + 一键静音）
 *
 * 从 ui_home.c 抽出。模块负责弹窗内容与交互，
 * 分区视觉刷新/事件记录/蜂鸣器静音通过 hooks 交回主页，
 * 避免面板之间互相依赖。
 */
#ifndef UI_ALARM_H
#define UI_ALARM_H

#include "lvgl/lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /** 某分区确认消警后（刷新分区卡片视觉） */
    void (*on_zone_ack)(int zone);
    /** 全部告警确认完成（记事件 + toast） */
    void (*on_confirmed)(void);
    /** 点击 MUTE（停鸣 + 记事件） */
    void (*on_muted)(void);
} ui_alarm_hooks_t;

/** 注入 hooks（在 build 之前调用） */
void ui_alarm_set_hooks(const ui_alarm_hooks_t *hooks);

/** 创建弹窗（初始隐藏） */
void ui_alarm_build(void);

/** 触发告警时弹出：显式传入分区名（模块不依赖分区表） */
void ui_alarm_open(int zone_idx, const char *zone_name, const char *src);

/** 弹窗当前是否可见 */
int ui_alarm_visible(void);

/** 隐藏弹窗（云端复核自动消警时调用） */
void ui_alarm_hide(void);

/** 同步告警状态机（ACK→CLEAR→TIMEOUT），供人工/自动消警复用 */
void ui_alarm_clear_fsm(int zone);

#ifdef __cplusplus
}
#endif

#endif /* UI_ALARM_H */
