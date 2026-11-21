/**
 * ui_album.h — 抓拍相册 + 查看器
 *
 * 从 ui_home.c 抽出：相册网格（3×2）、全图查看器、抓拍缓冲与 TF 恢复。
 * 抓拍来源：手动抓拍按钮、预警触发（ui_events）。
 */
#ifndef UI_ALBUM_H
#define UI_ALBUM_H

#ifdef __cplusplus
extern "C" {
#endif

/** 创建相册与查看器（初始隐藏） */
void ui_album_build(void);

/** 启动时从 TF 恢复最近抓拍 */
void ui_album_restore(void);

/** 打开相册（title 用于弹窗标题，可为 NULL） */
void ui_album_open(const char *title);

int ui_album_visible(void);
int ui_album_viewer_visible(void);

/**
 * 抓拍当前摄像头帧并存入相册。
 * @param zone_idx 分区索引（用于 TF 文件名编码）
 * @param zone     分区名（显示用）
 * @param level    级别 high/medium/low（空=无）
 */
void ui_album_capture(int zone_idx, const char *zone, const char *level);

/** 手动抓拍：分区由摄像头归属决定 */
void ui_album_capture_current(const char *zone, const char *level);

#ifdef __cplusplus
}
#endif

#endif /* UI_ALBUM_H */
