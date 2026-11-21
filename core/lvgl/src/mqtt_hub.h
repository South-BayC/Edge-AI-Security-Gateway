/**
 * mqtt_hub.h — 云端事件上报（T6：常驻线程 + 重连 + 本地缓存补传）
 *
 * 设计:
 *   - 无外部依赖：TCP socket 手工构造 MQTT 3.1.1 报文；
 *   - 事件先写入 event_store（JSONL，掉电不丢），再入有界队列；
 *   - 常驻网络线程串行发送，失败指数退避重连，成功后按事件 id 删除本地缓存；
 *   - 设备 ID 从 eth0 MAC 派生，topic = gateway/{device_id}/event；
 *   - UI 线程只入队 + 落盘，不阻塞。
 *
 * 配置（编译期）:
 *   MQTT_HUB_BROKER   broker IP/域名
 *   MQTT_HUB_PORT     端口（默认 1883）
 */
#ifndef MQTT_HUB_H
#define MQTT_HUB_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 初始化网络线程、加载本地待补传事件。
 * 在 config_init() 之后、UI 创建之前调用。
 * @return 0=成功；-1=线程创建失败（事件仍会落盘）
 */
int mqtt_hub_start(void);

/** 停止网络线程并关闭连接（退出时调用） */
void mqtt_hub_stop(void);

/**
 * 发布事件（异步：落盘 + 入队，立即返回）
 * @param type   触发类型（"SENSOR"/"CAM"）
 * @param zone   区域名（"前门" 等，UTF-8）
 * @param level  级别（"high"/"medium"/"low"）
 * @param time   时间 "HH:MM"
 */
void mqtt_hub_publish_event(const char *type, const char *zone,
                            const char *level, const char *time);

/** 当前本地待补传事件数 */
int mqtt_hub_pending(void);

/** 设备 ID（"gw-xxxxxxxxxxxx"） */
const char *mqtt_hub_device_id(void);

#ifdef __cplusplus
}
#endif

#endif /* MQTT_HUB_H */
