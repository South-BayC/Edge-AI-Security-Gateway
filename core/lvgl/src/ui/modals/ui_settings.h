/**
 * ui_settings.h — 设置弹窗（亮度 / 云端复核 / 预警阈值 / 定时布防 / 系统 / 关于）
 *
 * 从 ui_home.c 抽出。控件与回调由本模块持有；
 * 背光写寄存器、配置落盘、摄像头徽章等能力仍由 ui_home 提供
 * （通过 ui_internal.h 的 ui 层内部接口）。
 */
#ifndef UI_SETTINGS_H
#define UI_SETTINGS_H

#include "lvgl/lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 创建弹窗（初始隐藏） */
void ui_settings_build(void);

/** 打开设置弹窗 */
void ui_settings_open(void);

/** 弹窗当前是否可见 */
int ui_settings_visible(void);

/** 云端复核开关变化后刷新按钮视觉（on=1 开） */
void ui_settings_refresh_review(int on);

/** 运动检测参数变化后刷新数值标签 */
void ui_settings_refresh_motion(void);

/** 远程配置下发后同步设置页显示（亮度/定时布防时间等） */
void ui_settings_sync_from_config(void);

#ifdef __cplusplus
}
#endif

#endif /* UI_SETTINGS_H */
