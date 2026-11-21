/**
 * zone_state.h — 安防分区状态（领域层，无 UI / 无硬件依赖）
 *
 * 唯一状态源：UI、dev_bridge（硬件桥接）、告警声光都从这里读取，
 * 避免此前 UI 内部 g_zone_states 与 state_machine 各自维护一份。
 *
 * 线程约定：只允许在 UI 主线程调用（dev_bridge_poll / LVGL 定时器）。
 */
#ifndef ZONE_STATE_H
#define ZONE_STATE_H

#include "../zone_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 复位为全部 ONLINE（启动/测试用） */
void zone_state_init(void);

void zone_state_set(int id, zone_state_t st);
zone_state_t zone_state_get(int id);

int zone_state_is_armed(int id);
int zone_state_any_armed(void);
int zone_state_has_alarm(int id);
int zone_state_any_alarm(void);

/** 告警级别：0=无 1=低(门) 2=中(仓库) 3=高(窗/周界)，取最高 */
int zone_state_alarm_level(void);

/** 分区显示名（前门/后门/窗户/仓库）；越界返回 "" */
const char *zone_state_name(int id);

void zone_state_arm_all(void);
void zone_state_disarm_all(void);
/** 人工/云端确认：ALARM → ARMED */
void zone_state_ack(int id);

#ifdef __cplusplus
}
#endif

#endif /* ZONE_STATE_H */
