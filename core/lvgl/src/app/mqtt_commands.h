/**
 * mqtt_commands.h — 云端下行命令处理（app 层业务编排）
 *
 * 从 mqtt_hub.c 抽出的命令分发：
 *   set_config / set_warn  远程配置（写 config_mgr + 发 EV_SYS 通知 UI）
 *   get_config             回读当前配置
 *   ota                    下发 manifest URL（包走 HTTP 下载）
 *
 * 负责 ACK 与「同 id 重试去重」；协议收发在 infra/mqtt_client。
 */
#ifndef MQTT_COMMANDS_H
#define MQTT_COMMANDS_H

#ifdef __cplusplus
extern "C" {
#endif

/** 清空命令 id 去重缓存（启动时调用） */
void mqtt_commands_reset(void);

/**
 * 处理一条下行命令。
 * @param device_id 设备 ID（用于构造 gateway/<id>/cmd/ack）
 * @param payload   JSON 文本（不要求 NUL 结尾）
 * @param len       payload 长度
 */
void mqtt_commands_handle(const char *device_id, const char *payload, int len);

#ifdef __cplusplus
}
#endif

#endif /* MQTT_COMMANDS_H */
