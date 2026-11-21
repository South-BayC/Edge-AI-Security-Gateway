/**
 * mqtt_client.h — MQTT 3.1.1 协议客户端（infra 层）
 *
 * 只负责报文编码/解析与连接生命周期，不含业务：
 *   CONNECT（含遗嘱）/ PUBLISH QoS0 / SUBSCRIBE QoS0 / PINGREQ / DISCONNECT
 * 传输细节（TCP / mbedTLS）由 mqtt_transport 屏蔽。
 *
 * 线程约定：只允许网络线程调用。
 */
#ifndef MQTT_CLIENT_H
#define MQTT_CLIENT_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *host;
    int         port;
    int         tls;           /* 1=启用 TLS（编译期需 MQTT_TLS） */
    const char *ca_path;       /* NULL/空=不校验（仅调试） */
    const char *client_id;
    const char *user;          /* NULL/空=匿名 */
    const char *pass;
    int         keepalive_sec; /* <=0 默认 60 */
} mqtt_client_cfg_t;

/** 下行 PUBLISH 回调（topic/payload 均为临时缓冲，回调内需自行拷贝） */
typedef void (*mqtt_publish_cb_t)(const char *topic, const char *payload,
                                  int len, void *user);

/** 建立连接并完成 CONNACK 校验；0=成功 -1=失败 */
int  mqtt_client_connect(const mqtt_client_cfg_t *cfg);

/** 发送 DISCONNECT 并关闭连接（可重复调用） */
void mqtt_client_disconnect(void);

int  mqtt_client_connected(void);
int  mqtt_client_tls_active(void);

/** 发布 QoS0；0=成功 -1=失败 */
int  mqtt_client_publish(const char *topic, const char *payload);

/** 发送 PINGREQ（不等待 PINGRESP，由 poll 统一收包） */
int  mqtt_client_ping(void);

/** 订阅单 topic（QoS0，packet id 固定 1） */
int  mqtt_client_subscribe(const char *topic);

/**
 * 非阻塞收一个报文。
 * PUBLISH 通过 cb 交给业务层；SUBACK/PINGRESP 等控制包在此消费。
 * @return 0=正常 -1=检测到断线（内部已断开）
 */
int  mqtt_client_poll(mqtt_publish_cb_t cb, void *user);

#ifdef __cplusplus
}
#endif

#endif /* MQTT_CLIENT_H */
