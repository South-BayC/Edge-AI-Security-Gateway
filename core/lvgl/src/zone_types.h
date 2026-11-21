/**
 * zone_types.h — 安防分区状态（中立类型）
 *
 * 只定义状态枚举，不包含任何状态机实现。
 * UI 与事件层共享该类型；实现逻辑在 alarm_fsm / ui_events 中。
 */
#ifndef ZONE_TYPES_H
#define ZONE_TYPES_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ZONE_ONLINE = 0,   /* 待机/在线（未布防） */
    ZONE_ARMED,        /* 布防中 */
    ZONE_ALARM,        /* 告警中 */
    ZONE_OFFLINE       /* 离线/异常 */
} zone_state_t;

/** 安防分区数量（固定 4：前门/后门/窗户/仓库） */
#define ZONE_COUNT 4

#ifdef __cplusplus
}
#endif

#endif /* ZONE_TYPES_H */
