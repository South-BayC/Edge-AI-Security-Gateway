/**
 * health_mgr.c — 单实例锁 + 看门狗 + 线程健康心跳实现（T8）
 */
#include "health_mgr.h"

#include "log_svc.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <linux/watchdog.h>

#define HEALTH_LOCK_PATHS { \
    "/tmp/lvgl_gateway.lock", \
    "/var/run/lvgl_gateway.lock", \
    NULL \
}

static const char *s_names[HEALTH_MAX] = {
    "MAIN", "BRIDGE", "CAM", "NET"
};

static uint32_t s_timeout_ms[HEALTH_MAX] = {
    2000, 3000, 5000, 10000
};

static uint64_t s_last_ms[HEALTH_MAX];
static int s_seen[HEALTH_MAX];
static int s_bad[HEALTH_MAX];

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static int s_inited = 0;
static int s_fault_logged = 0;

static int s_lock_fd = -1;
static char s_lock_path[64];
static volatile int s_restart_req = 0;
static int s_wd_fd = -1;

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000L);
}

/* 返回 0=拿到锁；-2=已有实例；-1=没有可用锁路径（降级）
 * 用 O_CREAT|O_EXCL 原子创建，避免 NFS 上 flock 不可靠。 */
static int open_instance_lock(void)
{
    const char *paths[] = HEALTH_LOCK_PATHS;

    for (int i = 0; paths[i]; i++) {
        for (int retry = 0; retry < 2; retry++) {
            int fd = open(paths[i], O_CREAT | O_EXCL | O_RDWR, 0644);
            if (fd >= 0) {
                char buf[32];
                int len = snprintf(buf, sizeof(buf), "%d\n", (int)getpid());
                if (write(fd, buf, (size_t)len) < 0)
                    LOGW("HEALTH", "write lock pid failed: %s", strerror(errno));

                s_lock_fd = fd;
                strncpy(s_lock_path, paths[i], sizeof(s_lock_path) - 1);
                s_lock_path[sizeof(s_lock_path) - 1] = 0;
                LOGI("HEALTH", "single instance lock %s (pid=%d)",
                     s_lock_path, (int)getpid());
                return 0;
            }

            if (errno != EEXIST)
                break;      /* 路径不可用，尝试下一个 */

            /* 文件已存在：读 pid，进程还活着就拒绝 */
            int rfd = open(paths[i], O_RDONLY);
            if (rfd < 0)
                break;

            char pidbuf[32];
            int n = read(rfd, pidbuf, sizeof(pidbuf) - 1);
            close(rfd);

            pid_t pid = 0;
            if (n > 0) {
                pidbuf[n] = 0;
                pid = (pid_t)atoi(pidbuf);
            }

            if (pid <= 0 || (pid != getpid() && kill(pid, 0) == 0)) {
                /* 空文件（对方刚创建还没写 pid）或进程仍存活：拒绝 */
                strncpy(s_lock_path, paths[i], sizeof(s_lock_path) - 1);
                s_lock_path[sizeof(s_lock_path) - 1] = 0;
                return -2;
            }

            /* stale（进程已退出）：删除后重试创建 */
            unlink(paths[i]);
        }
    }

    return -1;
}

static void open_watchdog(void)
{
    const char *env = getenv("GATEWAY_WATCHDOG");
    if (env && strcmp(env, "0") == 0) {
        LOGI("HEALTH", "watchdog disabled by GATEWAY_WATCHDOG=0");
        return;
    }

    int fd = open("/dev/watchdog", O_WRONLY);
    if (fd < 0) {
        LOGW("HEALTH", "/dev/watchdog open failed: %s (watchdog disabled)",
             strerror(errno));
        return;
    }

    int timeout = 30;
    if (ioctl(fd, WDIOC_SETTIMEOUT, &timeout) != 0)
        LOGW("HEALTH", "WDIOC_SETTIMEOUT failed: %s", strerror(errno));

    s_wd_fd = fd;
    LOGI("HEALTH", "watchdog enabled, timeout=%ds", timeout);
}

int health_init(void)
{
    if (s_inited)
        return 0;

    int lock_ret = open_instance_lock();
    if (lock_ret == -2) {
        LOGE("HEALTH", "another instance is running (%s)", s_lock_path);
        return -2;
    }
    if (lock_ret == -1)
        LOGW("HEALTH", "no writable lock path, single-instance check disabled");

    open_watchdog();

    uint64_t now = now_ms();
    pthread_mutex_lock(&s_lock);
    for (int i = 0; i < HEALTH_MAX; i++) {
        s_last_ms[i] = now;
        s_seen[i] = 0;
        s_bad[i] = 0;
    }
    s_seen[HEALTH_MAIN] = 1;
    pthread_mutex_unlock(&s_lock);

    s_inited = 1;
    return (s_wd_fd >= 0) ? 0 : -1;
}

void health_beat(health_id_t id)
{
    if (id < 0 || id >= HEALTH_MAX)
        return;

    pthread_mutex_lock(&s_lock);
    s_last_ms[id] = now_ms();
    s_seen[id] = 1;
    pthread_mutex_unlock(&s_lock);
}

int health_tick(void)
{
    uint64_t now = now_ms();
    int bad = 0;

    pthread_mutex_lock(&s_lock);
    s_last_ms[HEALTH_MAIN] = now;
    s_seen[HEALTH_MAIN] = 1;

    for (int i = 1; i < HEALTH_MAX; i++) {
        if (!s_seen[i]) {
            s_bad[i] = 0;
            continue;
        }
        if (now - s_last_ms[i] > s_timeout_ms[i]) {
            s_bad[i] = 1;
            bad = 1;
        } else {
            s_bad[i] = 0;
        }
    }

    if (bad && !s_fault_logged) {
        for (int i = 1; i < HEALTH_MAX; i++) {
            if (s_bad[i]) {
                LOGE("HEALTH", "%s heartbeat timeout (>%ums)",
                     s_names[i], s_timeout_ms[i]);
            }
        }
        s_fault_logged = 1;
    } else if (!bad) {
        s_fault_logged = 0;
    }
    pthread_mutex_unlock(&s_lock);

    if (bad)
        return 0;

    if (s_wd_fd >= 0)
        ioctl(s_wd_fd, WDIOC_KEEPALIVE, 0);

    return 1;
}

void health_shutdown(void)
{
    if (s_wd_fd >= 0) {
        /* magic close：告诉驱动是正常退出，不再复位 */
        if (write(s_wd_fd, "V", 1) < 0)
            LOGW("HEALTH", "watchdog magic close failed: %s", strerror(errno));
        close(s_wd_fd);
        s_wd_fd = -1;
    }

    if (s_lock_fd >= 0) {
        flock(s_lock_fd, LOCK_UN);
        close(s_lock_fd);
        s_lock_fd = -1;
        if (s_lock_path[0])
            unlink(s_lock_path);
    }

    s_inited = 0;
}

void health_request_restart(void)
{
    s_restart_req = 1;
}

int health_take_restart(void)
{
    if (!s_restart_req)
        return 0;
    s_restart_req = 0;
    return 1;
}

int health_watchdog_enabled(void)
{
    return s_wd_fd >= 0;
}
