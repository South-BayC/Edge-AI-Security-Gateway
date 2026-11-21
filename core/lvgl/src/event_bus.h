/**
 * event_bus.h — 统一事件总线（T5）
 *
 * 设计：
 *   - 固定大小有界队列（64 条），线程安全；
 *   - 事件带优先级 P0~P4，队列满时优先保留高优先级；
 *   - 业务线程 publish，主线程 poll 后统一调用 ui_events_*；
 *   - 事件结构固定大小，避免动态内存和悬空指针。
 *
 * 当前接入：告警触发/消警、传感器故障/恢复、OTA 状态等。
 */
#ifndef EVENT_BUS_H
#define EVENT_BUS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    EV_SYS = 0,      /* 系统/校准/健康 */
    EV_SENSOR,       /* 传感器数据质量 */
    EV_MOTION,       /* 摄像头运动 */
    EV_ALARM,        /* 告警触发/消警 */
    EV_NET,          /* 网络/在线状态 */
    EV_STORAGE,      /* 存储 */
    EV_OTA,          /* OTA 状态 */
    EV_TYPE_MAX
} event_type_t;

typedef enum {
    EV_PRIO_P0 = 0,  /* 系统严重故障 */
    EV_PRIO_P1,      /* 高等级安防告警 */
    EV_PRIO_P2,      /* 普通运动 */
    EV_PRIO_P3,      /* 传感器状态 */
    EV_PRIO_P4       /* 调试/状态 */
} event_prio_t;

typedef struct {
    uint32_t id;
    event_type_t type;
    event_prio_t prio;
    uint64_t ts_ms;
    int source;          /* zone/sensor id */
    int code;            /* 事件码：告警 1=触发 0=消警；EV_SYS 2=日志 3=toast 4=用户活动 5=配置变更 */
    char text[96];       /* 附注（触发源/原因/标题） */
    char loc[24];        /* 日志位置（前门/系统/云端…） */
    char level[12];      /* 日志级别（high/medium/low/空） */
} system_event_t;

/** 初始化/清空队列 */
void event_bus_init(void);

/**
 * 发布事件（线程安全，非阻塞）。
 * 队列满时：新事件优先级更高则替换最低优先级事件，否则丢弃。
 * @return 0=入队/替换；-1=丢弃
 */
int event_bus_publish(const system_event_t *ev);

/**
 * 取一条事件（主线程轮询，按优先级从高到低）。
 * @return 1=取到；0=空
 */
int event_bus_poll(system_event_t *out);

/** 当前队列条数 */
int event_bus_pending(void);

#ifdef __cplusplus
}
#endif

#endif /* EVENT_BUS_H */
