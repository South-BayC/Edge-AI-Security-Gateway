/**
 * ui_sensor.h — 检测页（模块状态/校准/曲线/系统信息/终端）
 */
#ifndef UI_SENSOR_H
#define UI_SENSOR_H

#include "lvgl/lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 创建检测弹窗（初始隐藏） */
void ui_sensor_build(void);

/** 打开检测弹窗（回到卡片网格并刷新一次） */
void ui_sensor_open(void);

/** 弹窗当前是否可见 */
int ui_sensor_visible(void);

/** 1s 节拍：刷新卡片数值/状态（内部判断是否可见） */
void ui_sensor_tick_1s(void);

/** 10Hz 节拍：采样缓冲 + 曲线推进 + 子页数值刷新 */
void ui_sensor_tick_10hz(void);

#ifdef __cplusplus
}
#endif

#endif /* UI_SENSOR_H */
