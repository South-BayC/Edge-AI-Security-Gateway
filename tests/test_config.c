/**
 * test_config.c — 配置持久化测试（D1）
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config_mgr.h"

static int fails;

#define CHECK(cond, name) do { \
    if (!(cond)) { printf("FAIL: %s\n", name); fails++; } \
    else { printf("ok  : %s\n", name); } \
} while (0)

static int read_all(const char *path, char *buf, int n)
{
    FILE *fp = fopen(path, "r");
    if (!fp) return -1;
    int r = (int)fread(buf, 1, (size_t)n - 1, fp);
    fclose(fp);
    buf[r] = 0;
    return r;
}

int main(void)
{
    const char *path = "/tmp/gw-test/cfg/gateway.json";
    char buf[8192];
    FILE *fp = NULL;

    system("rm -rf /tmp/gw-test");

    /* 1. 默认值 */
    struct app_config d;
    config_defaults(&d);
    CHECK(d.motion_diff == 24, "default motion_diff=24");
    CHECK(d.warn.fusion_on == 0, "default fusion_on=0");
    CHECK(d.warn.fusion_threshold == 70, "default fusion_threshold=70");
    CHECK(strcmp(d.mqtt_broker, "192.168.3.100") == 0, "default broker");
    CHECK(d.mqtt_port == 1883 && d.mqtt_tls == 0, "default mqtt plain 1883");
    CHECK(d.camera_zone == 0, "default camera_zone=0");
    CHECK(d.warn.zones[0].threshold_warn == 20, "default pre-warn ps=20");
    CHECK(d.warn.zones[3].channel == IMU_DATA_LIN, "default zone3 uses lin");
    CHECK(d.warn.zones[3].threshold == 30 && d.warn.zones[3].threshold_warn == 15,
          "default zone3 30/15");
    CHECK(d.temp_k[0] == 0 && d.temp_ref == 0, "default no temp drift fit");

    /* 2. 文件不存在 → 默认 */
    CHECK(config_init(path) == 0, "config_init");
    const struct app_config *c = config_get();
    CHECK(c->motion_diff == 24, "loaded default diff");

    /* 3. 修改 + 落盘 */
    struct app_config m = *c;
    m.motion_diff = 20;
    m.warn.fusion_on = 1;
    m.warn.fusion_threshold = 80;
    snprintf(m.mqtt_broker, sizeof(m.mqtt_broker), "192.168.3.26");
    snprintf(m.mqtt_user, sizeof(m.mqtt_user), "gateway");
    snprintf(m.mqtt_pass, sizeof(m.mqtt_pass), "secret");
    snprintf(m.cloud_api_key, sizeof(m.cloud_api_key), "k123");
    m.mqtt_port = 8883;
    m.mqtt_tls = 1;
    m.camera_zone = 3;
    snprintf(m.mqtt_ca_path, sizeof(m.mqtt_ca_path), "/opt/edge-gateway/certs/ca.crt");
    m.warn.zones[0].threshold_warn = 18;
    m.temp_k[0] = 1200; m.temp_k[1] = -800; m.temp_k[2] = 0;
    m.temp_ref = 9000;
    m.calib_pt_n = 2;
    m.calib_pt_temp[0] = 7000; m.calib_pt_temp[1] = 9000;
    m.calib_pt_off[0][0] = 11; m.calib_pt_off[0][1] = 21;

    CHECK(config_update(&m) == 0, "config_update");
    CHECK(config_flush() == 0, "config_flush");

    CHECK(read_all(path, buf, sizeof(buf)) > 0, "config file written");
    CHECK(strstr(buf, "\"diff\": 20") != NULL, "json has diff=20");
    CHECK(strstr(buf, "\"mqtt_broker\": \"192.168.3.26\"") != NULL, "json has broker");
    CHECK(strstr(buf, "\"fusion_on\": 1") != NULL, "json has fusion_on");
    CHECK(strstr(buf, "\"cloud_api_key\": \"k123\"") != NULL, "json has api key");
    CHECK(strstr(buf, "\"threshold_warn\": 18") != NULL, "json has pre-warn threshold");
    CHECK(strstr(buf, "\"mqtt_port\": 8883") != NULL, "json has mqtt_port");
    CHECK(strstr(buf, "\"mqtt_tls\": 1") != NULL, "json has mqtt_tls");
    CHECK(strstr(buf, "\"mqtt_ca_path\": \"/opt/edge-gateway/certs/ca.crt\"") != NULL,
          "json has mqtt ca path");
    CHECK(strstr(buf, "\"camera_zone\": 3") != NULL, "json has camera zone");
    CHECK(strstr(buf, "\"temp_k\": [1200, -800, 0]") != NULL, "json has temp_k");
    CHECK(strstr(buf, "\"pts\": 2") != NULL, "json has calib points");

    /* 4. 重新加载 */
    CHECK(config_init(path) == 0, "reload");
    c = config_get();
    CHECK(c->motion_diff == 20, "reloaded diff");
    CHECK(c->warn.fusion_on == 1, "reloaded fusion_on");
    CHECK(strcmp(c->mqtt_user, "gateway") == 0, "reloaded user");
    CHECK(strcmp(c->cloud_api_key, "k123") == 0, "reloaded api key");
    CHECK(c->warn.zones[0].threshold_warn == 18, "reloaded pre-warn threshold");
    CHECK(c->mqtt_port == 8883 && c->mqtt_tls == 1, "reloaded mqtt tls");
    CHECK(c->camera_zone == 3, "reloaded camera zone");
    CHECK(strcmp(c->mqtt_ca_path, "/opt/edge-gateway/certs/ca.crt") == 0,
          "reloaded mqtt ca path");
    CHECK(c->temp_k[0] == 1200 && c->temp_k[1] == -800, "reloaded temp_k");
    CHECK(c->calib_pt_n == 2 && c->calib_pt_off[0][1] == 21, "reloaded calib points");

    /* 5. 越界裁剪 */
    struct app_config bad = *c;
    bad.motion_diff = 999;
    bad.warn.fusion_threshold = -5;
    bad.warn.fusion_window_ms = 99999;
    bad.warn.zones[0].threshold = 10;
    bad.warn.zones[0].threshold_warn = 50;   /* 预警高于告警 → 收敛到告警值 */
    config_update(&bad);
    c = config_get();
    CHECK(c->motion_diff == 100, "clamp motion_diff");
    CHECK(c->warn.fusion_threshold == 1, "clamp fusion_threshold");
    CHECK(c->warn.fusion_window_ms == 10000, "clamp fusion_window");
    CHECK(c->warn.zones[0].threshold_warn == 10, "pre-warn clamped to alarm");

    /* 5b. v1 配置迁移：IMU 阈值 150（合加速度）→ 30/15（去重力线性加速度） */
    fp = fopen(path, "w");
    if (fp) {
        fputs("{\"version\":1,\"warn\":{\"debounce\":2,\"cooldown_sec\":60,"
              "\"zones\":["
              "{\"src\":2,\"channel\":0,\"threshold\":150,\"cmp\":0,\"enabled\":1},"
              "{\"src\":0,\"channel\":0,\"threshold\":0,\"cmp\":0,\"enabled\":0},"
              "{\"src\":0,\"channel\":0,\"threshold\":0,\"cmp\":0,\"enabled\":0},"
              "{\"src\":2,\"channel\":0,\"threshold\":150,\"cmp\":0,\"enabled\":1}]}}",
              fp);
        fclose(fp);
    }
    CHECK(config_init(path) == 0, "init v1 config");
    c = config_get();
    CHECK(c->version == 2, "version bumped to 2");
    CHECK(c->warn.zones[0].threshold == 30, "v1 migrate alarm 150->30");
    CHECK(c->warn.zones[0].threshold_warn == 15, "v1 migrate pre-warn=15");

    /* 6. 损坏 JSON → 默认值 */
    fp = fopen(path, "w");
    if (fp) { fputs("{ this is not json", fp); fclose(fp); }
    CHECK(config_init(path) == 0, "init with corrupt file");
    c = config_get();
    CHECK(c->motion_diff == 24, "corrupt -> defaults");

    printf("%s\n", fails ? "RESULT: FAIL" : "RESULT: PASS");
    return fails ? 1 : 0;
}
