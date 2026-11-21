/**
 * mqtt_hub.c — MQTT 事件上报（T6：常驻线程 + 重连 + 本地缓存补传）
 *
 * 数据流：
 *   UI/业务线程
 *     → mqtt_hub_publish_event()
 *         → event_store_append()     （先落盘，掉电不丢）
 *         → 有界队列 + cond signal
 *   net_thread
 *     → 取队首 → 连接（必要时）→ PUBLISH
 *         → 成功：event_store_remove(id) + 出队
 *         → 失败：保持队首，指数退避后重试（事件已在本地缓存）
 *     → 空闲时每 30s PINGREQ 保活；失败则断开重连
 */
#include "mqtt_hub.h"
#include "infra/mqtt_client.h"
#include "event_store.h"
#include "log_svc.h"
#include "health_mgr.h"
#include "config_mgr.h"
#include "ota_mgr.h"
#include "thread_util.h"
#include "app/mqtt_commands.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/* ---- 编译期配置 ---- */
#define MQTT_HUB_ENABLED     1
#define MQTT_HUB_BROKER      "192.168.3.100"
#define MQTT_HUB_PORT        1883
#define MQTT_HUB_CLIENT      "imx6ull-gateway"
#define MQTT_IO_TIMEOUT_MS   800

#define MQ_QUEUE_MAX         64
#define MQ_PAYLOAD_MAX       384
#define MQ_BACKOFF_MIN_MS    1000
#define MQ_BACKOFF_MAX_MS    60000

typedef struct {
    char payload[MQ_PAYLOAD_MAX];
    char id[64];
} mq_msg_t;

static mq_msg_t s_q[MQ_QUEUE_MAX];
static int s_head = 0, s_tail = 0, s_count = 0;
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_cond = PTHREAD_COND_INITIALIZER;
static pthread_t s_tid;
static volatile int s_quit = 0;
static volatile int s_started = 0;
static char s_client_id[32];
static char s_broker[64] = MQTT_HUB_BROKER;
static int  s_port = MQTT_HUB_PORT;          /* T11：1883 明文 / 8883 TLS */
static int  s_tls = 0;                       /* T11：1=启用 TLS（需编译进 mbedTLS） */
static char s_ca[128] = "/opt/edge-gateway/certs/ca.crt";
static char s_user[48];                      /* T11：MQTT 用户名（空=匿名） */
static char s_pass[96];                      /* T11：MQTT 密码 */
static time_t s_last_conn_log = 0;
static unsigned long s_seq = 0;
static long s_boot_ts = 0;
static unsigned long s_dropped = 0;

/* 命令去重缓存与分发见 app/mqtt_commands（mqtt_commands_*） */

/* 报文编码/连接/发布见 infra/mqtt_client（mqtt_client_*） */

/* 组装连接参数（broker/端口/TLS/账号来自 mqtt_hub_start 的解析结果） */
static int hub_connect(void)
{
    mqtt_client_cfg_t cfg;
    cfg.host = s_broker;
    cfg.port = s_port;
    cfg.tls = s_tls;
    cfg.ca_path = s_ca;
    cfg.client_id = s_client_id;
    cfg.user = s_user;
    cfg.pass = s_pass;
    cfg.keepalive_sec = 60;
    return mqtt_client_connect(&cfg);
}

static void mqtt_publish_online(int online)
{
    char topic[64];
    char payload[128];
    snprintf(topic, sizeof(topic), "gateway/%s/status", s_client_id);
    snprintf(payload, sizeof(payload),
             "{\"online\":%s,\"device_id\":\"%s\",\"ts\":%ld}",
             online ? "true" : "false", s_client_id, (long)time(NULL));
    mqtt_client_publish(topic, payload);
}

/* T11-B3：设备影子（reported）——上报当前实际配置，云端无需主动查询。
 * 不含密码/API Key 等敏感字段。 */
static int mqtt_publish_shadow(void)
{
    const struct app_config *c = config_get();
    if (!c)
        return -1;

    char topic[64];
    char payload[512];
    snprintf(topic, sizeof(topic), "gateway/%s/shadow/reported", s_client_id);
    snprintf(payload, sizeof(payload),
             "{\"device_id\":\"%s\",\"ts\":%ld,\"version\":\"%s\",\"reported\":{"
             "\"cloud_review\":%d,\"motion_interval\":%d,\"motion_diff\":%d,"
             "\"brightness\":%d,\"schedule_on\":%d,"
             "\"sched_start_h\":%d,\"sched_end_h\":%d,"
             "\"debounce\":%d,\"cooldown_sec\":%d,"
             "\"mqtt_broker\":\"%s\",\"mqtt_user\":\"%s\"}}",
             s_client_id, (long)time(NULL), ota_current_version(),
             c->cloud_review, c->motion_interval, c->motion_diff,
             c->brightness, c->schedule_on,
             c->sched_start_h, c->sched_end_h,
             c->warn.debounce, c->warn.cooldown_sec,
             c->mqtt_broker, c->mqtt_user);

    return mqtt_client_publish(topic, payload);
}

/* 下行 PUBLISH → 命令分发（协议解析在 infra/mqtt_client，业务在 app/mqtt_commands） */
static void on_publish(const char *topic, const char *payload, int len, void *user)
{
    (void)user;
    if (topic && strstr(topic, "/cmd"))
        mqtt_commands_handle(s_client_id, payload, len);
}

/* 非阻塞读取下行包（PUBLISH/SUBACK/PINGRESP） */
static void mqtt_read_incoming(void)
{
    mqtt_client_poll(on_publish, NULL);
}

/* ---- 工具 ---- */

static void load_device_id(void)
{
    char mac[64] = {0};
    FILE *fp = fopen("/sys/class/net/eth0/address", "r");
    if (fp) {
        if (!fgets(mac, sizeof(mac), fp))
            mac[0] = 0;
        fclose(fp);
    }

    char clean[16];
    int n = 0;
    for (char *p = mac; *p && n < 12; p++) {
        if (isxdigit((unsigned char)*p))
            clean[n++] = (char)tolower((unsigned char)*p);
    }
    clean[n] = 0;

    if (n == 12)
        snprintf(s_client_id, sizeof(s_client_id), "gw-%s", clean);
    else
        snprintf(s_client_id, sizeof(s_client_id), "%s", MQTT_HUB_CLIENT);

    s_boot_ts = (long)time(NULL);
}

static void make_id(char *out, int n)
{
    snprintf(out, (size_t)n, "%s-%ld-%lu",
             s_client_id, s_boot_ts, s_seq++);
}

static void extract_id(const char *line, char *out, int n)
{
    out[0] = 0;
    const char *p = strstr(line, "\"id\":\"");
    if (!p)
        return;
    p += 6;
    int i = 0;
    while (*p && *p != '"' && i < n - 1)
        out[i++] = *p++;
    out[i] = 0;
}

/* 调用方持 s_lock */
static void enqueue_locked(const char *payload, const char *id)
{
    if (s_count >= MQ_QUEUE_MAX) {
        /* 不丢队首（可能正在重试）；新事件已经写入 event_store，
         * 连接恢复后由 reload_store() 重新入队补传。 */
        s_dropped++;
        return;
    }

    strncpy(s_q[s_tail].payload, payload, MQ_PAYLOAD_MAX - 1);
    s_q[s_tail].payload[MQ_PAYLOAD_MAX - 1] = 0;
    strncpy(s_q[s_tail].id, id, sizeof(s_q[s_tail].id) - 1);
    s_q[s_tail].id[sizeof(s_q[s_tail].id) - 1] = 0;

    s_tail = (s_tail + 1) % MQ_QUEUE_MAX;
    s_count++;
}

/* 调用方持 s_lock */
static int queue_has_id_locked(const char *id)
{
    for (int i = 0; i < s_count; i++) {
        int idx = (s_head + i) % MQ_QUEUE_MAX;
        if (strcmp(s_q[idx].id, id) == 0)
            return 1;
    }
    return 0;
}

/* 把 event_store 中尚未入队的事件重新加入发送队列（连接恢复后补传） */
static void reload_store(void)
{
    int n = event_store_count();
    for (int i = 0; i < n; i++) {
        char line[512];
        char id[64];

        if (event_store_get(i, line, sizeof(line)) != 0)
            continue;
        extract_id(line, id, sizeof(id));
        if (!id[0])
            continue;

        pthread_mutex_lock(&s_lock);
        if (!queue_has_id_locked(id)) {
            enqueue_locked(line, id);
            pthread_cond_signal(&s_cond);
        }
        pthread_mutex_unlock(&s_lock);
    }
}

/* ---- 网络线程 ---- */
static void *net_thread(void *arg)
{
    (void)arg;
    int backoff = MQ_BACKOFF_MIN_MS;
    time_t last_ping = time(NULL);
    time_t last_conn_try = 0;
    uint32_t last_shadow_gen = 0;
    int force_shadow = 1;          /* 连接后强制上报一次 */

    while (!s_quit) {
        mq_msg_t msg;
        int have = 0;

        /* T8：网络线程心跳（最长 1s 一次；看门狗超时 5s） */
        health_beat(HEALTH_NET);

        /* T7/T10：先处理下行命令（PUBLISH/SUBACK/PINGRESP） */
        mqtt_read_incoming();

        pthread_mutex_lock(&s_lock);
        while (s_count == 0 && !s_quit) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 200 * 1000 * 1000;   /* 200ms：兼顾功耗与命令延迟 */
            if (ts.tv_nsec >= 1000000000L) {
                ts.tv_sec++;
                ts.tv_nsec -= 1000000000L;
            }

            pthread_cond_timedwait(&s_cond, &s_lock, &ts);   /* 等待时自动释放锁 */
            pthread_mutex_unlock(&s_lock);

            /* 等待期间也要打心跳 + 读下行命令：
             * 否则 MQTT 命令最多要等 1s 才被处理，体验上像“没反应”。 */
            health_beat(HEALTH_NET);
            mqtt_read_incoming();

            pthread_mutex_lock(&s_lock);
            if (s_count == 0 && mqtt_client_connected() &&
                time(NULL) - last_ping >= 30)
                break;
            /* 未连接且空闲：每 5s 回外层尝试重连，保持在线状态 */
            if (s_count == 0 && !mqtt_client_connected() &&
                time(NULL) - last_conn_try >= 5)
                break;
        }

        if (s_quit && s_count == 0) {
            pthread_mutex_unlock(&s_lock);
            break;
        }

        if (s_count > 0) {
            memcpy(&msg, &s_q[s_head], sizeof(msg));
            have = 1;
        }
        pthread_mutex_unlock(&s_lock);

        /* 未连接时先重连（空闲也要保持在线，不等有事件才连） */
        if (!mqtt_client_connected()) {
            time_t now = time(NULL);
            if (now - last_conn_try < 5)
                continue;
            last_conn_try = now;

            if (hub_connect() != 0) {
                if (now - s_last_conn_log >= 10) {
                    LOGW("MQTT", "connect %s:%d failed", s_broker, s_port);
                    s_last_conn_log = now;
                }
                for (int i = 0; i < backoff && !s_quit; i += 100) {
                    health_beat(HEALTH_NET);   /* 退避期间线程仍存活 */
                    usleep(100 * 1000);
                }
                backoff *= 2;
                if (backoff > MQ_BACKOFF_MAX_MS)
                    backoff = MQ_BACKOFF_MAX_MS;
                continue;
            }

            /* 连接建立：订阅命令、发布在线状态、补传离线事件 */
            char cmd_topic[64];
            snprintf(cmd_topic, sizeof(cmd_topic), "gateway/%s/cmd", s_client_id);
            mqtt_client_subscribe(cmd_topic);
            mqtt_publish_online(1);
            reload_store();
            last_ping = time(NULL);
            force_shadow = 1;
            LOGI("MQTT", "connected %s:%d%s, subscribe+online",
                 s_broker, s_port, mqtt_client_tls_active() ? " (TLS)" : "");
        }

        /* T11-B3：连接后 / 配置变更时上报设备影子 */
        if (mqtt_client_connected()) {
            uint32_t gen = config_generation();
            if (force_shadow || gen != last_shadow_gen) {
                if (mqtt_publish_shadow() == 0) {
                    last_shadow_gen = gen;
                    force_shadow = 0;
                    LOGI("MQTT", "shadow reported (gen=%lu)", (unsigned long)gen);
                } else {
                    mqtt_client_disconnect();
                    force_shadow = 1;
                }
            }
        }

        if (!have) {
            if (time(NULL) - last_ping >= 30) {
                if (mqtt_client_ping() != 0)
                    mqtt_client_disconnect();
                last_ping = time(NULL);
            }
            continue;
        }

        char topic[64];
        snprintf(topic, sizeof(topic), "gateway/%s/event", s_client_id);

        if (mqtt_client_publish(topic, msg.payload) != 0) {
            mqtt_client_disconnect();
            for (int i = 0; i < backoff && !s_quit; i += 100) {
                health_beat(HEALTH_NET);   /* 退避期间线程仍存活，避免看门狗误判 */
                usleep(100 * 1000);
            }
            backoff *= 2;
            if (backoff > MQ_BACKOFF_MAX_MS)
                backoff = MQ_BACKOFF_MAX_MS;
            continue;
        }

        /* 发送成功：删除本地缓存并出队 */
        event_store_remove(msg.id);

        pthread_mutex_lock(&s_lock);
        s_head = (s_head + 1) % MQ_QUEUE_MAX;
        s_count--;
        pthread_mutex_unlock(&s_lock);

        backoff = MQ_BACKOFF_MIN_MS;
        last_ping = time(NULL);

        /* 队列有空位时，把因队列满而只留在 event_store 的事件补入 */
        if (event_store_count() > 0)
            reload_store();
    }

    if (mqtt_client_connected())
        mqtt_publish_online(0);
    mqtt_client_disconnect();
    return NULL;
}

/* ---- 对外接口 ---- */
int mqtt_hub_start(void)
{
#if MQTT_HUB_ENABLED
    if (s_started)
        return 0;

    mqtt_commands_reset();

    const char *env_broker = getenv("GATEWAY_MQTT_BROKER");
    if (env_broker && env_broker[0]) {
        snprintf(s_broker, sizeof(s_broker), "%s", env_broker);
    } else {
        /* 未用环境变量时读持久化配置（gateway.json 的 mqtt_broker） */
        const struct app_config *cfg = config_get();
        if (cfg && cfg->mqtt_broker[0])
            snprintf(s_broker, sizeof(s_broker), "%s", cfg->mqtt_broker);
    }

    /* T11：用户名/密码（环境变量优先，便于临时切换） */
    const struct app_config *cfg_auth = config_get();
    const char *env_user = getenv("GATEWAY_MQTT_USER");
    const char *env_pass = getenv("GATEWAY_MQTT_PASS");

    if (env_user && env_user[0])
        snprintf(s_user, sizeof(s_user), "%s", env_user);
    else if (cfg_auth && cfg_auth->mqtt_user[0])
        snprintf(s_user, sizeof(s_user), "%s", cfg_auth->mqtt_user);

    if (env_pass && env_pass[0])
        snprintf(s_pass, sizeof(s_pass), "%s", env_pass);
    else if (cfg_auth && cfg_auth->mqtt_pass[0])
        snprintf(s_pass, sizeof(s_pass), "%s", cfg_auth->mqtt_pass);

    /* T11-③：TLS 配置（环境变量优先；默认 1883 明文） */
    const struct app_config *cfg_tls = config_get();
    const char *env_tls  = getenv("GATEWAY_MQTT_TLS");
    const char *env_port = getenv("GATEWAY_MQTT_PORT");
    const char *env_ca   = getenv("GATEWAY_MQTT_CA");

    s_tls = (cfg_tls && cfg_tls->mqtt_tls) ? 1 : 0;
    if (cfg_tls && cfg_tls->mqtt_port > 0)
        s_port = cfg_tls->mqtt_port;
    if (cfg_tls && cfg_tls->mqtt_ca_path[0])
        snprintf(s_ca, sizeof(s_ca), "%s", cfg_tls->mqtt_ca_path);

    if (env_tls)
        s_tls = (atoi(env_tls) != 0);
    if (env_port && atoi(env_port) > 0)
        s_port = atoi(env_port);
    if (env_ca && env_ca[0])
        snprintf(s_ca, sizeof(s_ca), "%s", env_ca);

    if (s_user[0])
        LOGI("MQTT", "auth enabled (user=%s, pass=%s)",
             s_user, s_pass[0] ? "***" : "-");
    LOGI("MQTT", "transport: %s port=%d ca=%s",
         s_tls ? "TLS" : "plain", s_port, s_tls ? s_ca : "-");

    load_device_id();
    event_store_init(NULL);

    int n = event_store_count();
    reload_store();

    s_quit = 0;
    if (thread_start(&s_tid, 256 * 1024, net_thread, NULL) != 0) {
        LOGE("MQTT", "thread create failed");
        return -1;
    }
    s_started = 1;
    LOGI("MQTT", "start client=%s pending=%d broker=%s:%d (%s)",
         s_client_id, n, s_broker, s_port, s_tls ? "TLS" : "plain");
    return 0;
#else
    return 0;
#endif
}

void mqtt_hub_stop(void)
{
#if MQTT_HUB_ENABLED
    if (!s_started)
        return;

    pthread_mutex_lock(&s_lock);
    s_quit = 1;
    pthread_cond_broadcast(&s_cond);
    pthread_mutex_unlock(&s_lock);

    pthread_join(s_tid, NULL);
    s_started = 0;

    event_store_close();

    LOGI("MQTT", "stopped, pending=%d dropped=%lu",
         event_store_count(), s_dropped);
#endif
}

void mqtt_hub_publish_event(const char *type, const char *zone,
                            const char *level, const char *time_str)
{
#if MQTT_HUB_ENABLED
    char id[64];
    char payload[MQ_PAYLOAD_MAX];
    char type_e[32], zone_e[64], level_e[16], time_e[16];

    make_id(id, sizeof(id));
    json_escape(type, type_e, sizeof(type_e));
    json_escape(zone, zone_e, sizeof(zone_e));
    json_escape(level, level_e, sizeof(level_e));
    json_escape(time_str, time_e, sizeof(time_e));

    snprintf(payload, sizeof(payload),
             "{\"id\":\"%s\",\"device_id\":\"%s\",\"type\":\"%s\","
             "\"zone\":\"%s\",\"level\":\"%s\",\"time\":\"%s\",\"ts\":%ld}",
             id, s_client_id, type_e, zone_e, level_e, time_e,
             (long)time(NULL));

    /* 先落盘：断网/掉电都不丢；发送成功后按 id 删除 */
    event_store_append(payload);

    pthread_mutex_lock(&s_lock);
    enqueue_locked(payload, id);
    pthread_cond_signal(&s_cond);
    pthread_mutex_unlock(&s_lock);
#else
    (void)type; (void)zone; (void)level; (void)time_str;
#endif
}

int mqtt_hub_pending(void)
{
#if MQTT_HUB_ENABLED
    return event_store_count();
#else
    return 0;
#endif
}

const char *mqtt_hub_device_id(void)
{
    return s_client_id;
}
