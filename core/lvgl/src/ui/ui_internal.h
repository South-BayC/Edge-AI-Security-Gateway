/**
 * ui_internal.h — UI 层内部共享符号（仅限 ui/ 目录内部使用）
 *
 * 拆分 ui_home.c 后，各子模块（ui_events.c、ui_terminal.c…）需要访问
 * 主页构建出来的 LVGL 对象与内部刷新函数。这些符号**不是**对外 API：
 *   - 只允许 ui/ 下的文件包含本头文件；
 *   - domain / infra / app 不得包含。
 *
 * 设计说明：为避免跨模块直接读写状态，这里只暴露「UI 渲染与提示」能力，
 * 业务状态仍由 domain（zone_state / event_bus）持有。
 */
#ifndef UI_INTERNAL_H
#define UI_INTERNAL_H

#include "lvgl/lvgl.h"
#include "../zone_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 云端复核开关（UI 侧镜像，真值在 config_mgr） ---- */
#define CLOUD_REVIEW_ON   1
#define CLOUD_REVIEW_OFF  0

/* ---- 分区静态信息（展示用） ---- */
typedef struct {
    const char *name;
    const char *id;
    const char *sensor;
    const char *ip;
    const char *fw;
    const char *rssi;
} zone_info_t;

extern const zone_info_t zone_info[ZONE_COUNT];

/* ---- 主页共享对象 ---- */

/* ---- 主页共享状态 ---- */
extern int g_cloud_review;     /* CLOUD_REVIEW_ON/OFF */
extern int g_brightness;       /* 0~100 */
extern int g_schedule_on;      /* 1=定时布防启用 */
extern int g_sched_start_h;    /* 0~23 */
extern int g_sched_end_h;      /* 0~23 */
extern int g_zone_card_idx;    /* 摄像头卡片当前防区 */
extern int g_idle_sec;         /* 无操作计时（息屏） */

/* ---- 主页内部刷新/提示能力（定义在 ui_home.c） ---- */
void _update_zone_visual(int i);
void _add_event(const char *title, const char *loc, uint32_t dot_clr, const char *level);
void _show_toast(const char *txt);
void _build_zone_card(void);
void _review_refresh(void);
void _motion_param_refresh(void);
void _backlight_set(int percent);
int  _backlight_get(void);
void _cfg_save_ui(void);
void _screen_set_blank(int on);

#ifdef __cplusplus
}
#endif

#endif /* UI_INTERNAL_H */
