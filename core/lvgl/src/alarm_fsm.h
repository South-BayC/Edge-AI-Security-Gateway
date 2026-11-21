/**
 * alarm_fsm.h — 分区告警状态机（T5）
 *
 * 状态：NORMAL → PENDING → ACTIVE → MUTED → RECOVERY → NORMAL
 *
 * 与现有 UI 的关系：
 *   - 业务层（dev_bridge）通过 dispatch() 驱动状态；
 *   - 状态变化回调在主线程执行，可映射到 ui_events_*；
 *   - 人工消警由 UI 长按调用 dispatch(ACK)；
 *   - 自动消警/云端复核调用 dispatch(CLEAR)。
 */
#ifndef ALARM_FSM_H
#define ALARM_FSM_H

#ifdef __cplusplus
extern "C" {
#endif

#define ALARM_ZONE_MAX 4

typedef enum {
    ALARM_NORMAL = 0,
    ALARM_PENDING,
    ALARM_ACTIVE,
    ALARM_MUTED,
    ALARM_RECOVERY
} alarm_state_t;

typedef enum {
    ALARM_EV_TRIGGER = 0,   /* 传感器/陌生人/入侵触发 */
    ALARM_EV_CONFIRM,       /* 规则确认（现有语义：立即升级 ACTIVE） */
    ALARM_EV_ACK,           /* 人工消警 */
    ALARM_EV_MUTE,          /* 静音 */
    ALARM_EV_CLEAR,         /* 自动消警/事件结束 */
    ALARM_EV_TIMEOUT        /* 恢复超时 */
} alarm_event_t;

typedef void (*alarm_fsm_cb)(int zone, alarm_state_t old_state,
                             alarm_state_t new_state,
                             const char *reason, void *arg);

void alarm_fsm_init(void);
void alarm_fsm_set_callback(alarm_fsm_cb cb, void *arg);

/**
 * 派发事件；状态变化时调用回调。
 * @return 1=状态变化；0=无变化/参数错误
 */
int alarm_fsm_dispatch(int zone, alarm_event_t ev, const char *reason);

alarm_state_t alarm_fsm_state(int zone);

/** 该分区是否处于告警存在状态（PENDING/ACTIVE/MUTED） */
int alarm_fsm_alarming(int zone);

/** 是否有任一分区告警存在 */
int alarm_fsm_any_alarming(void);

const char *alarm_state_name(alarm_state_t st);

#ifdef __cplusplus
}
#endif

#endif /* ALARM_FSM_H */
