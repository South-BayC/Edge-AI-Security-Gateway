/**
 * health_mgr.h — 单实例锁 + 看门狗 + 线程健康心跳（T8）
 *
 * 职责：
 *   - 单实例：flock 锁文件，阻止重复启动（避免两个进程同时写日志/设备）；
 *   - 看门狗：打开 /dev/watchdog，主循环按健康状态喂狗；
 *   - 心跳：各常驻线程周期性 health_beat()，超时未喂 → 停止喂狗 → 30s 复位；
 *   - 可选模块（摄像头）未启动时不参与判断，避免未接设备导致复位。
 *
 * 使用：
 *   main:  health_init() → 主循环每 500ms health_tick() → health_shutdown()
 *   bridge: dev_bridge_poll() 中 health_beat(HEALTH_BRIDGE)
 *   cam:    采集循环中 health_beat(HEALTH_CAM)
 *   net:    mqtt 线程循环中 health_beat(HEALTH_NET)
 */
#ifndef HEALTH_MGR_H
#define HEALTH_MGR_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    HEALTH_MAIN = 0,
    HEALTH_BRIDGE,
    HEALTH_CAM,
    HEALTH_NET,
    HEALTH_MAX
} health_id_t;

/**
 * 初始化单实例锁与看门狗。
 * @return 0=成功；-1=看门狗不可用但锁成功（降级）；-2=已有实例，调用方应退出
 */
int health_init(void);

/** 更新指定模块心跳（线程安全，可在任意线程调用） */
void health_beat(health_id_t id);

/**
 * 主循环周期调用（建议 500ms）：更新主心跳、检查超时、喂狗。
 * @return 1=健康；0=有模块超时（已停止喂狗）
 */
int health_tick(void);

/** 退出：关闭看门狗（magic close）并释放单实例锁 */
void health_shutdown(void);

/**
 * 请求重启应用（UI 调用）：主循环退出 → 优雅清理 → exec 自身。
 * 会先释放单实例锁与看门狗，避免重启被自己挡住。
 */
void health_request_restart(void);

/** 主循环轮询：有重启请求则返回 1 并清除标记 */
int health_take_restart(void);

/** 看门狗是否启用 */
int health_watchdog_enabled(void);

#ifdef __cplusplus
}
#endif

#endif /* HEALTH_MGR_H */
