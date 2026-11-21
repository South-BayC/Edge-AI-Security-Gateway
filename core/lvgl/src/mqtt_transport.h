/**
 * mqtt_transport.h — MQTT 传输层抽象（T11 单向 TLS）
 *
 * 目的：把 socket 细节从 mqtt_hub.c 抽出来，TLS 开关对协议逻辑透明。
 *
 *   - 未定义 MQTT_TLS：纯 TCP（1883），行为与旧版一致；
 *   - 定义 MQTT_TLS：mbedTLS 客户端（8883），验证服务器证书 + 主机名。
 *
 * 线程约定：只允许网络线程调用（内部持有全局 fd / TLS 上下文）。
 */
#ifndef MQTT_TRANSPORT_H
#define MQTT_TRANSPORT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 建立连接。
 * @param host     broker 地址（IP 或域名）
 * @param port     端口（1883/8883）
 * @param tls      1=启用 TLS（需编译时定义 MQTT_TLS）
 * @param ca_path  CA 证书路径（PEM）；NULL/空=不校验（仅调试用）
 * @param sni      证书主机名（通常与 host 相同）
 * @return fd（>=0）成功；-1 失败
 */
int  mqt_open(const char *host, int port, int tls,
              const char *ca_path, const char *sni);

/** 发送全部数据；返回 0 成功，-1 失败 */
int  mqt_send(const void *buf, size_t len);

/** 接收：>0=字节数；0=暂无数据/超时；-1=连接断开或错误 */
int  mqt_recv(void *buf, size_t len);

/** 可读检测：1=有数据（含 TLS 已解密缓冲）；0=超时；-1=错误 */
int  mqt_readable(int timeout_ms);

/** 关闭连接（TLS 会先发 close_notify） */
void mqt_close(void);

/** 当前连接是否走 TLS */
int  mqt_tls_active(void);

#ifdef __cplusplus
}
#endif

#endif /* MQTT_TRANSPORT_H */
