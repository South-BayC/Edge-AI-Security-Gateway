/**
 * time_sync.c — 最小 SNTP 客户端实现
 *
 * 协议：RFC 4330 SNTPv4 客户端模式（LI=0/VN=4/Mode=3），48 字节请求，
 *       取响应里的 Transmit Timestamp（第 40~47 字节）换算成 Unix 时间。
 * 说明：无 NTP 服务器时每 5 分钟重试，日志有 [TIME] 前缀；成功后 6 小时再同步。
 */
#include "time_sync.h"

#include "log_svc.h"
#include "thread_util.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define NTP_PORT          123
#define NTP_PKT_LEN       48
#define NTP_UNIX_OFFSET   2208988800UL   /* 1900-01-01 → 1970-01-01 */
#define SYNC_PERIOD_SEC   (6 * 3600)
#define RETRY_PERIOD_SEC  300
#define NTP_RECV_TIMEOUT  2              /* 秒 */

static char s_server[96] = "192.168.3.26";
static pthread_t s_tid;
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_cond = PTHREAD_COND_INITIALIZER;
static int s_running = 0;
static int s_stop = 0;
static int s_synced = 0;

/* 解析服务器地址（IP 优先，失败再走 DNS；板端无 DNS 时用 IP 即可） */
static int resolve(const char *host, struct sockaddr_in *sa)
{
    memset(sa, 0, sizeof(*sa));
    sa->sin_family = AF_INET;
    sa->sin_port = htons(NTP_PORT);

    if (inet_aton(host, &sa->sin_addr) != 0)
        return 0;

    struct hostent *he = gethostbyname(host);
    if (!he || !he->h_addr_list[0])
        return -1;

    memcpy(&sa->sin_addr, he->h_addr_list[0], sizeof(sa->sin_addr));
    return 0;
}

static int sntp_once(const char *server)
{
    struct sockaddr_in sa;
    if (resolve(server, &sa) != 0) {
        LOGW("TIME", "resolve %s failed", server);
        return -1;
    }

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return -1;

    struct timeval tv = { NTP_RECV_TIMEOUT, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    unsigned char pkt[NTP_PKT_LEN];
    memset(pkt, 0, sizeof(pkt));
    pkt[0] = 0x23;     /* LI=0, VN=4, Mode=3 (client) */

    if (sendto(fd, pkt, sizeof(pkt), 0, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        close(fd);
        return -1;
    }

    ssize_t n = recv(fd, pkt, sizeof(pkt), 0);
    close(fd);
    if (n < NTP_PKT_LEN)
        return -1;

    uint32_t secs = ((uint32_t)pkt[40] << 24) | ((uint32_t)pkt[41] << 16) |
                    ((uint32_t)pkt[42] << 8)  |  (uint32_t)pkt[43];
    uint32_t frac = ((uint32_t)pkt[44] << 24) | ((uint32_t)pkt[45] << 16) |
                    ((uint32_t)pkt[46] << 8)  |  (uint32_t)pkt[47];

    /* 合理范围：2020-01-01 之后（防止收到全 0 / 异常包） */
    if (secs < NTP_UNIX_OFFSET + 1577836800UL) {
        LOGW("TIME", "invalid ntp timestamp");
        return -1;
    }

    struct timeval now;
    now.tv_sec  = (time_t)(secs - NTP_UNIX_OFFSET);
    now.tv_usec = (suseconds_t)(((uint64_t)frac * 1000000ULL) >> 32);

    if (settimeofday(&now, NULL) != 0) {
        LOGE("TIME", "settimeofday failed: %s", strerror(errno));
        return -1;
    }

    LOGI("TIME", "synced from %s (%ld)", server, (long)now.tv_sec);
    return 0;
}

static void wait_or_stop(int seconds)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += seconds;

    pthread_mutex_lock(&s_lock);
    if (!s_stop)
        pthread_cond_timedwait(&s_cond, &s_lock, &ts);
    pthread_mutex_unlock(&s_lock);
}

static void *sync_thread(void *arg)
{
    (void)arg;

    while (1) {
        pthread_mutex_lock(&s_lock);
        int stop = s_stop;
        pthread_mutex_unlock(&s_lock);
        if (stop)
            break;

        if (sntp_once(s_server) == 0) {
            s_synced = 1;
            wait_or_stop(SYNC_PERIOD_SEC);
        } else {
            if (s_synced) {
                /* 已同步过，偶尔失败不必刷屏 */
                LOGW("TIME", "resync failed, retry in %ds", RETRY_PERIOD_SEC);
            } else {
                LOGW("TIME", "sync %s failed, retry in %ds",
                     s_server, RETRY_PERIOD_SEC);
            }
            wait_or_stop(RETRY_PERIOD_SEC);
        }
    }
    return NULL;
}

void time_sync_start(const char *server)
{
    if (s_running)
        return;

    const char *env = getenv("GATEWAY_NTP_SERVER");
    if (server && server[0])
        snprintf(s_server, sizeof(s_server), "%s", server);
    else if (env && env[0])
        snprintf(s_server, sizeof(s_server), "%s", env);

    s_stop = 0;
    if (thread_start(&s_tid, 128 * 1024, sync_thread, NULL) != 0) {
        LOGE("TIME", "sync thread create failed");
        return;
    }
    s_running = 1;
    LOGI("TIME", "sntp started, server=%s", s_server);
}

void time_sync_stop(void)
{
    if (!s_running)
        return;

    pthread_mutex_lock(&s_lock);
    s_stop = 1;
    pthread_cond_broadcast(&s_cond);
    pthread_mutex_unlock(&s_lock);

    pthread_join(s_tid, NULL);
    s_running = 0;
    LOGI("TIME", "sntp stopped");
}
