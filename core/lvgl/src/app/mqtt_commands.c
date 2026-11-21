/**
 * mqtt_commands.c — 云端下行命令处理实现
 */
#include "mqtt_commands.h"

#include "../config_mgr.h"
#include "../event_bus.h"
#include "../log_svc.h"
#include "../ota_mgr.h"
#include "../domain/json_util.h"
#include "../infra/mqtt_client.h"

#include <stdio.h>
#include <string.h>

/* T12：命令 id 去重（云端 ACK 超时重试时，避免重复执行/重复触发 OTA） */
#define CMD_ID_CACHE 16
static int s_cmd_ids[CMD_ID_CACHE];
static int s_cmd_id_idx = 0;

void mqtt_commands_reset(void)
{
    memset(s_cmd_ids, 0, sizeof(s_cmd_ids));
    s_cmd_id_idx = 0;
}

/* 远程配置：更新 config_mgr 并发布 EV_SYS，UI 主线程刷新 */
static void remote_apply_config(const char *json)
{
    struct app_config c = *config_get();
    int v;

    if (json_get_int(json, "cloud_review", &v))    c.cloud_review = v;
    if (json_get_int(json, "motion_interval", &v)) c.motion_interval = v;
    if (json_get_int(json, "motion_diff", &v))     c.motion_diff = v;
    if (json_get_int(json, "camera_zone", &v))     c.camera_zone = v;
    if (json_get_int(json, "brightness", &v))      c.brightness = v;
    if (json_get_int(json, "schedule_on", &v))     c.schedule_on = v;
    if (json_get_int(json, "sched_start_h", &v))   c.sched_start_h = v;
    if (json_get_int(json, "sched_end_h", &v))     c.sched_end_h = v;
    if (json_get_int(json, "debounce", &v))        c.warn.debounce = v;
    if (json_get_int(json, "cooldown_sec", &v))    c.warn.cooldown_sec = v;
    if (json_get_int(json, "fusion_on", &v))       c.warn.fusion_on = v;
    if (json_get_int(json, "fusion_threshold", &v)) c.warn.fusion_threshold = v;
    if (json_get_int(json, "fusion_window_ms", &v)) c.warn.fusion_window_ms = v;
    if (json_get_int(json, "mqtt_port", &v))       c.mqtt_port = v;
    if (json_get_int(json, "mqtt_tls", &v))        c.mqtt_tls = v;

    /* 字符串项：MQTT 连接参数（下次启动生效） */
    char s[128];
    if (json_get_str(json, "mqtt_broker", s, sizeof(s)) && s[0])
        snprintf(c.mqtt_broker, sizeof(c.mqtt_broker), "%s", s);
    if (json_get_str(json, "mqtt_user", s, sizeof(s)) && s[0])
        snprintf(c.mqtt_user, sizeof(c.mqtt_user), "%s", s);
    if (json_get_str(json, "mqtt_pass", s, sizeof(s)) && s[0])
        snprintf(c.mqtt_pass, sizeof(c.mqtt_pass), "%s", s);
    if (json_get_str(json, "cloud_api_key", s, sizeof(s)) && s[0])
        snprintf(c.cloud_api_key, sizeof(c.cloud_api_key), "%s", s);
    if (json_get_str(json, "mqtt_ca_path", s, sizeof(s)) && s[0])
        snprintf(c.mqtt_ca_path, sizeof(c.mqtt_ca_path), "%s", s);

    int zone;
    if (json_get_int(json, "zone", &zone)) {
        if (zone < 0 || zone > 3)
            zone = 0;
        if (json_get_int(json, "src", &v))
            c.warn.zones[zone].src = v;
        if (json_get_int(json, "channel", &v))
            c.warn.zones[zone].channel = v;
        if (json_get_int(json, "threshold", &v))
            c.warn.zones[zone].threshold = v;
        if (json_get_int(json, "threshold_warn", &v))
            c.warn.zones[zone].threshold_warn = v;
        if (json_get_int(json, "cmp", &v))
            c.warn.zones[zone].cmp = v;
        if (json_get_int(json, "enabled", &v))
            c.warn.zones[zone].enabled = v;
    }

    config_update(&c);
    config_flush();

    system_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = EV_SYS;
    ev.prio = EV_PRIO_P3;
    ev.code = 1;                  /* 1=远程配置变更 */
    snprintf(ev.text, sizeof(ev.text), "remote config");
    event_bus_publish(&ev);
}

static void cmd_send_ack(const char *device_id, int id, const char *cmd, int ok)
{
    char topic[64];
    char ack[160];
    snprintf(topic, sizeof(topic), "gateway/%s/cmd/ack", device_id ? device_id : "");
    snprintf(ack, sizeof(ack), "{\"id\":%d,\"cmd\":\"%s\",\"ok\":%s}",
             id, (cmd && cmd[0]) ? cmd : "unknown", ok ? "true" : "false");
    mqtt_client_publish(topic, ack);
}

void mqtt_commands_handle(const char *device_id, const char *payload, int len)
{
    char buf[512];
    char cmd[32];
    int id = 0;
    int ok = 1;

    if (!payload || len <= 0)
        return;
    if (len >= (int)sizeof(buf))
        len = sizeof(buf) - 1;
    memcpy(buf, payload, (size_t)len);
    buf[len] = 0;

    json_get_str(buf, "cmd", cmd, sizeof(cmd));
    json_get_int(buf, "id", &id);

    /* 重试幂等：同一 id 已成功执行过，只补 ACK，不再执行 */
    if (id > 0) {
        for (int i = 0; i < CMD_ID_CACHE; i++) {
            if (s_cmd_ids[i] == id) {
                cmd_send_ack(device_id, id, cmd, 1);
                LOGI("MQTT", "cmd=%s id=%d duplicate -> ack only", cmd, id);
                return;
            }
        }
    }

    if (strcmp(cmd, "set_config") == 0) {
        remote_apply_config(buf);
    } else if (strcmp(cmd, "set_warn") == 0) {
        remote_apply_config(buf);
    } else if (strcmp(cmd, "get_config") == 0) {
        const struct app_config *c = config_get();
        char reply[512];
        snprintf(reply, sizeof(reply),
                 "{\"cloud_review\":%d,\"motion_interval\":%d,"
                 "\"motion_diff\":%d,\"brightness\":%d,"
                 "\"schedule_on\":%d,\"mqtt_broker\":\"%s\",\"mqtt_user\":\"%s\"}",
                 c->cloud_review, c->motion_interval, c->motion_diff,
                 c->brightness, c->schedule_on,
                 c->mqtt_broker, c->mqtt_user);
        char topic[64];
        snprintf(topic, sizeof(topic), "gateway/%s/cmd/ack", device_id ? device_id : "");
        mqtt_client_publish(topic, reply);
        return;
    } else if (strcmp(cmd, "ota") == 0) {
        /* T9+：MQTT 下发 OTA（只传 manifest URL，包仍走 HTTP 下载） */
        char url[256];
        if (!json_get_str(buf, "url", url, sizeof(url)) || !url[0] ||
            ota_request_check(url) != 0)
            ok = 0;
    } else {
        ok = 0;
    }

    cmd_send_ack(device_id, id, cmd, ok);
    if (id > 0 && ok) {
        s_cmd_ids[s_cmd_id_idx] = id;
        s_cmd_id_idx = (s_cmd_id_idx + 1) % CMD_ID_CACHE;
    }

    LOGI("MQTT", "cmd=%s id=%d ok=%d", cmd[0] ? cmd : "?", id, ok);
}
