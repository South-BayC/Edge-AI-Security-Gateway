/**
 * mqtt_client.c — MQTT 3.1.1 协议客户端实现
 */
#include "mqtt_client.h"

#include "../mqtt_transport.h"
#include "../health_mgr.h"
#include "../log_svc.h"

#include <stdio.h>
#include <string.h>

#define MQTT_IO_TIMEOUT_MS   800
#define MQTT_DEF_KEEPALIVE   60

static int s_fd = -1;

/* ---- 报文基础 ---- */
static int mqtt_encode_len(unsigned char *buf, int len)
{
    int n = 0;
    do {
        unsigned char d = (unsigned char)(len % 128);
        len /= 128;
        if (len > 0)
            d |= 0x80;
        buf[n++] = d;
    } while (len > 0);
    return n;
}

static int mqtt_write_str(unsigned char *buf, const char *s)
{
    int len = (int)strlen(s);
    buf[0] = (unsigned char)(len >> 8);
    buf[1] = (unsigned char)(len & 0xFF);
    memcpy(buf + 2, s, (size_t)len);
    return len + 2;
}

int mqtt_client_connected(void)
{
    return s_fd >= 0;
}

int mqtt_client_tls_active(void)
{
    return mqt_tls_active();
}

int mqtt_client_connect(const mqtt_client_cfg_t *cfg)
{
    if (!cfg || !cfg->host || !cfg->client_id)
        return -1;

    /* T8：连接过程（TCP + CONNACK）可能因网络/冷启动变慢，
     * 在关键阻塞点前后补心跳，避免误判 NET 卡死。 */
    health_beat(HEALTH_NET);

    int fd = mqt_open(cfg->host, cfg->port, cfg->tls,
                      cfg->ca_path, cfg->host);
    if (fd < 0)
        return -1;

    health_beat(HEALTH_NET);

    unsigned char pkt[320];
    unsigned char vb[256];
    int n = 0, vn = 0;

    char will_topic[64];
    snprintf(will_topic, sizeof(will_topic), "gateway/%s/status", cfg->client_id);

    const char *user = cfg->user ? cfg->user : "";
    const char *pass = cfg->pass ? cfg->pass : "";

    /* 可选的用户名/密码认证（CONNECT flags bit7/bit6） */
    unsigned char flags = 0x06;                 /* clean session + will */
    if (user[0]) {
        flags |= 0x80;
        if (pass[0])
            flags |= 0x40;
    }

    int keepalive = (cfg->keepalive_sec > 0) ? cfg->keepalive_sec : MQTT_DEF_KEEPALIVE;
    if (keepalive > 255)
        keepalive = 255;

    vn += mqtt_write_str(vb + vn, "MQTT");
    vb[vn++] = 0x04;              /* protocol level 3.1.1 */
    vb[vn++] = flags;
    vb[vn++] = (unsigned char)(keepalive >> 8);
    vb[vn++] = (unsigned char)(keepalive & 0xFF);
    vn += mqtt_write_str(vb + vn, cfg->client_id);
    vn += mqtt_write_str(vb + vn, will_topic);
    vn += mqtt_write_str(vb + vn, "{\"online\":false}");
    if (user[0]) {
        vn += mqtt_write_str(vb + vn, user);
        if (pass[0])
            vn += mqtt_write_str(vb + vn, pass);
    }

    unsigned char rl[4];
    int rln = mqtt_encode_len(rl, vn);
    pkt[n++] = 0x10;
    memcpy(pkt + n, rl, (size_t)rln); n += rln;
    memcpy(pkt + n, vb, (size_t)vn); n += vn;

    if (mqt_send(pkt, (size_t)n) != 0) {
        mqt_close();
        return -1;
    }

    unsigned char connack[4];
    int rn = mqt_recv(connack, sizeof(connack));
    if (rn < 4 || connack[0] != 0x20 || connack[3] != 0) {
        health_beat(HEALTH_NET);
        mqt_close();
        return -1;
    }

    s_fd = fd;
    health_beat(HEALTH_NET);
    return 0;
}

void mqtt_client_disconnect(void)
{
    if (s_fd >= 0) {
        unsigned char d[2] = { 0xE0, 0x00 };
        mqt_send(d, 2);
        mqt_close();
        s_fd = -1;
    }
}

int mqtt_client_publish(const char *topic, const char *payload)
{
    if (s_fd < 0)
        return -1;

    unsigned char vb[768];
    int vn = mqtt_write_str(vb, topic);
    int plen = (int)strlen(payload);
    if (vn + plen >= (int)sizeof(vb))
        return -1;
    memcpy(vb + vn, payload, (size_t)plen);
    vn += plen;

    unsigned char rl[4];
    int rln = mqtt_encode_len(rl, vn);

    unsigned char pkt[800];
    int n = 0;
    pkt[n++] = 0x30;              /* PUBLISH QoS0 */
    memcpy(pkt + n, rl, (size_t)rln); n += rln;
    memcpy(pkt + n, vb, (size_t)vn); n += vn;

    return mqt_send(pkt, (size_t)n);
}

int mqtt_client_ping(void)
{
    if (s_fd < 0)
        return -1;

    /* 只发 PINGREQ，不在这里等 PINGRESP：
     * 网络线程用 mqtt_client_poll() 统一收包，PINGRESP 会被它先消费掉，
     * 若这里再阻塞 recv 会必然超时并误判断线（每 30s 掉一次连接）。 */
    unsigned char req[2] = { 0xC0, 0x00 };
    return mqt_send(req, 2);
}

int mqtt_client_subscribe(const char *topic)
{
    if (s_fd < 0)
        return -1;

    unsigned char vb[128];
    int vn = 0;
    vb[vn++] = 0x00;
    vb[vn++] = 0x01;              /* packet id = 1 */
    vn += mqtt_write_str(vb + vn, topic);
    vb[vn++] = 0x00;              /* requested QoS0 */

    unsigned char rl[4];
    int rln = mqtt_encode_len(rl, vn);

    unsigned char pkt[160];
    int n = 0;
    pkt[n++] = 0x82;              /* SUBSCRIBE */
    memcpy(pkt + n, rl, (size_t)rln); n += rln;
    memcpy(pkt + n, vb, (size_t)vn); n += vn;

    return mqt_send(pkt, (size_t)n);
}

int mqtt_client_poll(mqtt_publish_cb_t cb, void *user)
{
    if (s_fd < 0)
        return -1;

    int rr = mqt_readable(0);
    if (rr < 0) {
        mqtt_client_disconnect();
        return -1;
    }
    if (rr == 0)
        return 0;

    unsigned char hdr;
    int n = mqt_recv(&hdr, 1);
    if (n != 1) {
        if (n < 0) {
            mqtt_client_disconnect();
            return -1;
        }
        return 0;
    }

    int rem = 0, mult = 1;
    unsigned char b;
    do {
        n = mqt_recv(&b, 1);
        if (n != 1) {
            if (n < 0) {
                mqtt_client_disconnect();
                return -1;
            }
            return 0;
        }
        rem += (b & 0x7F) * mult;
        mult *= 128;
    } while ((b & 0x80) && mult <= 128 * 128 * 128);

    if (rem <= 0 || rem > 512) {
        unsigned char tmp[128];
        int left = rem;
        while (left > 0) {
            int k = mqt_recv(tmp, left > (int)sizeof(tmp) ? (int)sizeof(tmp) : left);
            if (k <= 0)
                break;
            left -= k;
        }
        return 0;
    }

    unsigned char buf[512];
    int got = 0;
    while (got < rem) {
        int k = mqt_recv(buf + got, (size_t)(rem - got));
        if (k <= 0) {
            mqtt_client_disconnect();
            return -1;
        }
        got += k;
    }

    int type = hdr & 0xF0;
    if (type == 0x30 && cb) {           /* PUBLISH */
        int qos = (hdr >> 1) & 0x03;
        int pos = 0;
        int tlen = (buf[pos] << 8) | buf[pos + 1];
        pos += 2;
        if (tlen <= 0 || pos + tlen > rem)
            return 0;

        char topic[128];
        int copy_len = tlen;
        if (copy_len >= (int)sizeof(topic))
            copy_len = sizeof(topic) - 1;
        memcpy(topic, buf + pos, (size_t)copy_len);
        topic[copy_len] = 0;
        pos += tlen;
        if (qos > 0)
            pos += 2;
        if (pos > rem)
            return 0;

        cb(topic, (const char *)buf + pos, rem - pos, user);
    }

    return 0;
}
