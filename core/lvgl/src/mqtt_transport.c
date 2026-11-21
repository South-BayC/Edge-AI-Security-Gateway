/**
 * mqtt_transport.c — MQTT 传输层实现（T11）
 *
 * 明文：与原 mqtt_hub.c 的 socket 逻辑一致（非阻塞 connect + 2s 超时，
 *       之后阻塞 + 800ms 收发超时）。
 * TLS ：mbedTLS 2.28 客户端，阻塞式 BIO + 超时，握手 5s 上限；
 *       默认 VERIFY_REQUIRED（无 CA 时降级为不校验，仅用于本地调试）。
 *
 * 注意：mbedTLS 的“已解密但未读完”数据不在 socket 缓冲里，
 *       所以 mqt_readable() 先查 mbedtls_ssl_get_bytes_avail()。
 */
#include "mqtt_transport.h"

#include "log_svc.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#ifdef MQTT_TLS
#include <mbedtls/ssl.h>
#include <mbedtls/net_sockets.h>   /* MBEDTLS_ERR_NET_SEND_FAILED / RECV_FAILED / CONN_RESET */
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/error.h>
#endif

#define MQT_IO_TIMEOUT_MS        800
#define MQT_CONNECT_TIMEOUT_SEC  2
#define MQT_TLS_HANDSHAKE_MS     5000

static int s_fd = -1;
static int s_tls = 0;

#ifdef MQTT_TLS
static mbedtls_ssl_context      s_ssl;
static mbedtls_ssl_config       s_conf;
static mbedtls_x509_crt         s_ca;
static mbedtls_entropy_context  s_entropy;
static mbedtls_ctr_drbg_context s_drbg;
#endif

#ifdef MQTT_TLS
static uint64_t now_ms64(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000L);
}
#endif

static void set_io_timeout(int fd, int ms)
{
    struct timeval tv = { ms / 1000, (ms % 1000) * 1000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

/* ---- 明文 TCP ---- */
static int tcp_connect(const char *host, int port)
{
    char ip[64];
    strncpy(ip, host, sizeof(ip) - 1);
    ip[sizeof(ip) - 1] = 0;

    struct in_addr addr;
    if (inet_aton(ip, &addr) == 0) {
        struct hostent *he = gethostbyname(ip);
        if (!he)
            return -1;
        addr.s_addr = *(unsigned long *)he->h_addr_list[0];
    }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((unsigned short)port);
    sa.sin_addr = addr;

    /* 非阻塞 connect + 2s 超时：broker 不可达时不能把网络线程卡住 */
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0) {
        close(fd);
        return -1;
    }
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);

    int rc = connect(fd, (struct sockaddr *)&sa, sizeof(sa));
    if (rc < 0 && errno != EINPROGRESS) {
        close(fd);
        return -1;
    }
    if (rc < 0) {
        fd_set wset;
        FD_ZERO(&wset);
        FD_SET(fd, &wset);
        struct timeval ctv = { MQT_CONNECT_TIMEOUT_SEC, 0 };
        if (select(fd + 1, NULL, &wset, NULL, &ctv) <= 0) {
            close(fd);
            return -1;
        }
        int err = 0;
        socklen_t el = sizeof(err);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el) < 0 || err != 0) {
            close(fd);
            return -1;
        }
    }
    fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);

    set_io_timeout(fd, MQT_IO_TIMEOUT_MS);
    return fd;
}

static int plain_send_all(const void *buf, size_t len)
{
    const char *p = (const char *)buf;
    while (len > 0) {
        ssize_t n = send(s_fd, p, len, 0);
        if (n <= 0) {
            if (n < 0 && errno == EINTR)
                continue;
            return -1;
        }
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

/* ---- TLS ---- */
#ifdef MQTT_TLS
static int bio_send(void *ctx, const unsigned char *buf, size_t len)
{
    int fd = *(int *)ctx;
    ssize_t n = send(fd, buf, len, 0);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
            return MBEDTLS_ERR_SSL_WANT_WRITE;
        return MBEDTLS_ERR_NET_SEND_FAILED;
    }
    return (int)n;
}

static int bio_recv(void *ctx, unsigned char *buf, size_t len)
{
    int fd = *(int *)ctx;
    ssize_t n = recv(fd, buf, len, 0);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
            return MBEDTLS_ERR_SSL_WANT_READ;
        return MBEDTLS_ERR_NET_RECV_FAILED;
    }
    if (n == 0)
        return MBEDTLS_ERR_NET_CONN_RESET;
    return (int)n;
}

static void tls_free(void)
{
    mbedtls_ssl_free(&s_ssl);
    mbedtls_ssl_config_free(&s_conf);
    mbedtls_x509_crt_free(&s_ca);
    mbedtls_ctr_drbg_free(&s_drbg);
    mbedtls_entropy_free(&s_entropy);
}

static int tls_start(const char *ca_path, const char *sni)
{
    static const char pers[] = "edge-gw-mqtt";
    int ret;

    mbedtls_ssl_init(&s_ssl);
    mbedtls_ssl_config_init(&s_conf);
    mbedtls_x509_crt_init(&s_ca);
    mbedtls_entropy_init(&s_entropy);
    mbedtls_ctr_drbg_init(&s_drbg);

    if (ca_path && ca_path[0]) {
        ret = mbedtls_x509_crt_parse_file(&s_ca, ca_path);
        if (ret != 0) {
            LOGE("MQTT", "TLS: parse CA %s failed -0x%04X", ca_path, -ret);
            tls_free();
            return -1;
        }
    }

    ret = mbedtls_ctr_drbg_seed(&s_drbg, mbedtls_entropy_func, &s_entropy,
                                (const unsigned char *)pers, sizeof(pers) - 1);
    if (ret != 0) {
        LOGE("MQTT", "TLS: drbg seed failed -0x%04X", -ret);
        tls_free();
        return -1;
    }

    ret = mbedtls_ssl_config_defaults(&s_conf, MBEDTLS_SSL_IS_CLIENT,
                                      MBEDTLS_SSL_TRANSPORT_STREAM,
                                      MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret != 0) {
        LOGE("MQTT", "TLS: config defaults failed -0x%04X", -ret);
        tls_free();
        return -1;
    }

    /* 有 CA 就强制校验；没有则只加密（仅本地调试） */
    mbedtls_ssl_conf_authmode(&s_conf,
        (ca_path && ca_path[0]) ? MBEDTLS_SSL_VERIFY_REQUIRED
                                : MBEDTLS_SSL_VERIFY_NONE);
    mbedtls_ssl_conf_rng(&s_conf, mbedtls_ctr_drbg_random, &s_drbg);
    mbedtls_ssl_conf_ca_chain(&s_conf, &s_ca, NULL);
    mbedtls_ssl_conf_min_version(&s_conf, MBEDTLS_SSL_MAJOR_VERSION_3,
                                 MBEDTLS_SSL_MINOR_VERSION_3);   /* TLS 1.2 */

    ret = mbedtls_ssl_setup(&s_ssl, &s_conf);
    if (ret != 0) {
        LOGE("MQTT", "TLS: setup failed -0x%04X", -ret);
        tls_free();
        return -1;
    }

    if (sni && sni[0]) {
        ret = mbedtls_ssl_set_hostname(&s_ssl, sni);
        if (ret != 0) {
            LOGE("MQTT", "TLS: set hostname failed -0x%04X", -ret);
            tls_free();
            return -1;
        }
    }

    mbedtls_ssl_set_bio(&s_ssl, &s_fd, bio_send, bio_recv, NULL);

    uint64_t deadline = now_ms64() + MQT_TLS_HANDSHAKE_MS;
    do {
        ret = mbedtls_ssl_handshake(&s_ssl);
    } while ((ret == MBEDTLS_ERR_SSL_WANT_READ ||
              ret == MBEDTLS_ERR_SSL_WANT_WRITE) && now_ms64() < deadline);

    if (ret != 0) {
        uint32_t vr = mbedtls_ssl_get_verify_result(&s_ssl);
        char info[256] = {0};
        if (vr != 0)
            mbedtls_x509_crt_verify_info(info, sizeof(info), "  ", vr);
        LOGE("MQTT", "TLS: handshake failed -0x%04X verify=0x%08X%s%s",
             -ret, vr, info[0] ? "\n" : "", info);
        tls_free();
        return -1;
    }

    uint32_t vr = mbedtls_ssl_get_verify_result(&s_ssl);
    if (vr != 0) {
        LOGE("MQTT", "TLS: cert verify failed 0x%08X", vr);
        tls_free();
        return -1;
    }

    LOGI("MQTT", "TLS handshake ok (sni=%s, ca=%s)",
         (sni && sni[0]) ? sni : "-",
         (ca_path && ca_path[0]) ? ca_path : "none");
    return 0;
}
#endif /* MQTT_TLS */

/* ---- 对外接口 ---- */
int mqt_open(const char *host, int port, int tls,
             const char *ca_path, const char *sni)
{
    mqt_close();

    int fd = tcp_connect(host, port);
    if (fd < 0)
        return -1;
    s_fd = fd;
    s_tls = 0;

#ifdef MQTT_TLS
    if (tls) {
        if (tls_start(ca_path, sni) != 0) {
            mqt_close();
            return -1;
        }
        s_tls = 1;
    }
#else
    (void)ca_path;
    (void)sni;
    if (tls) {
        LOGE("MQTT", "TLS requested but binary built without mbedTLS");
        mqt_close();
        return -1;
    }
#endif
    return s_fd;
}

int mqt_send(const void *buf, size_t len)
{
    if (s_fd < 0)
        return -1;

#ifdef MQTT_TLS
    if (s_tls) {
        const unsigned char *p = (const unsigned char *)buf;
        int tries = 3;   /* 配合 SO_SNDTIMEO，避免 WANT_* 死循环 */
        while (len > 0) {
            int n = mbedtls_ssl_write(&s_ssl, p, len);
            if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE) {
                if (--tries <= 0)
                    return -1;
                continue;
            }
            if (n <= 0)
                return -1;
            p += n;
            len -= (size_t)n;
        }
        return 0;
    }
#endif
    return plain_send_all(buf, len);
}

int mqt_recv(void *buf, size_t len)
{
    if (s_fd < 0)
        return -1;

#ifdef MQTT_TLS
    if (s_tls) {
        int n = mbedtls_ssl_read(&s_ssl, (unsigned char *)buf, len);
        if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE)
            return 0;                      /* 超时/暂无数据 */
        if (n == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY)
            return -1;
        if (n <= 0)
            return -1;
        return n;
    }
#endif

    ssize_t n = recv(s_fd, buf, len, 0);
    if (n > 0)
        return (int)n;
    if (n == 0)
        return -1;
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
        return 0;
    return -1;
}

int mqt_readable(int timeout_ms)
{
    if (s_fd < 0)
        return -1;

#ifdef MQTT_TLS
    /* 已解密但未读走的数据不在 socket 缓冲里 */
    if (s_tls && mbedtls_ssl_get_bytes_avail(&s_ssl) > 0)
        return 1;
#endif

    fd_set rf;
    FD_ZERO(&rf);
    FD_SET(s_fd, &rf);
    struct timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
    int r = select(s_fd + 1, &rf, NULL, NULL, &tv);
    if (r > 0)
        return 1;
    if (r == 0)
        return 0;
    return -1;
}

void mqt_close(void)
{
#ifdef MQTT_TLS
    if (s_tls) {
        mbedtls_ssl_close_notify(&s_ssl);
        tls_free();
        s_tls = 0;
    }
#endif
    if (s_fd >= 0) {
        close(s_fd);
        s_fd = -1;
    }
}

int mqt_tls_active(void)
{
    return s_tls;
}
