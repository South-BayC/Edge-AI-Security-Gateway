/**
 * ui_time.h — UI 时间字符串（当日秒数 → HH:MM）
 *
 * 事件时间轴与抓拍元数据共用；当日秒数由主页 1s 定时器按系统时间刷新。
 */
#ifndef UI_TIME_H
#define UI_TIME_H

#ifdef __cplusplus
extern "C" {
#endif

/** 更新“当日秒数”（0..86399），由时钟定时器每秒调用 */
void ui_time_set_day_seconds(int sec);

/** 取当前 HH:MM（不足缓冲则截断） */
void ui_time_str(char *buf, int len);

#ifdef __cplusplus
}
#endif

#endif /* UI_TIME_H */
