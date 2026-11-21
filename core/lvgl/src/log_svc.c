/**
 * log_svc.c — 分级异步日志服务实现
 *
 * 结构：
 *   业务线程 --log_write()--> 固定大小内存队列 --> log_thread --> 文件轮转
 *                                              └--> stderr（串口调试）
 *
 * 队列满时丢弃低级别日志并计数，保证业务线程不被阻塞。
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE          /* localtime_r / strcasecmp */
#endif

#include "log_svc.h"
#include "thread_util.h"

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define LOG_LINE_MAX        384
#define LOG_QUEUE_LINES     64
#define LOG_DEFAULT_MAX_KB  256
#define LOG_DEFAULT_MAX_FILES 4
#define LOG_CONSOLE         1

static char s_dir[PATH_MAX];
static FILE *s_fp = NULL;
static long s_bytes = 0;
static long s_max_bytes = LOG_DEFAULT_MAX_KB * 1024;
static int  s_max_files = LOG_DEFAULT_MAX_FILES;

static char s_queue[LOG_QUEUE_LINES][LOG_LINE_MAX];
static int  s_head = 0;
static int  s_tail = 0;
static int  s_count = 0;
static unsigned long s_dropped = 0;

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  s_cond = PTHREAD_COND_INITIALIZER;
static pthread_t       s_tid;
static volatile int    s_inited = 0;
static volatile int    s_quit = 0;
static volatile int    s_level = LOG_INFO;

static const char *level_name(int lv)
{
    switch (lv) {
    case LOG_DEBUG: return "DEBUG";
    case LOG_INFO:  return "INFO";
    case LOG_WARN:  return "WARN";
    case LOG_ERROR: return "ERROR";
    case LOG_FATAL: return "FATAL";
    default:        return "?";
    }
}

static int level_parse(const char *s)
{
    if (!s) return LOG_INFO;
    if (strcasecmp(s, "debug") == 0) return LOG_DEBUG;
    if (strcasecmp(s, "info") == 0)  return LOG_INFO;
    if (strcasecmp(s, "warn") == 0)  return LOG_WARN;
    if (strcasecmp(s, "error") == 0) return LOG_ERROR;
    if (strcasecmp(s, "fatal") == 0) return LOG_FATAL;
    return LOG_INFO;
}

static int mkdir_p(const char *path, mode_t mode)
{
    if (!path || !path[0])
        return -1;

    char tmp[PATH_MAX];
    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = 0;

    size_t len = strlen(tmp);
    if (len == 0)
        return 0;
    if (tmp[len - 1] == '/')
        tmp[len - 1] = 0;

    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            if (mkdir(tmp, mode) != 0 && errno != EEXIST)
                return -1;
            *p = '/';
        }
    }
    if (mkdir(tmp, mode) != 0 && errno != EEXIST)
        return -1;
    return 0;
}

static void log_rotate(void)
{
    if (!s_fp)
        return;

    fflush(s_fp);
    fclose(s_fp);
    s_fp = NULL;

    char oldp[PATH_MAX], newp[PATH_MAX];

    /* 删除最旧，再依次后移 */
    for (int i = s_max_files - 1; i >= 1; i--) {
        snprintf(oldp, sizeof(oldp), "%s/gateway.log.%d", s_dir, i);
        snprintf(newp, sizeof(newp), "%s/gateway.log.%d", s_dir, i + 1);
        if (i == s_max_files - 1)
            remove(oldp);
        else
            rename(oldp, newp);
    }

    snprintf(oldp, sizeof(oldp), "%s/gateway.log", s_dir);
    snprintf(newp, sizeof(newp), "%s/gateway.log.1", s_dir);
    rename(oldp, newp);

    s_fp = fopen(oldp, "a");
    s_bytes = 0;
}

static void log_write_file(const char *line)
{
    size_t len = strlen(line);

    if (s_fp && s_max_bytes > 0 && s_bytes + (long)len > s_max_bytes)
        log_rotate();

    if (s_fp) {
        fwrite(line, 1, len, s_fp);
        fflush(s_fp);
        s_bytes += (long)len;
    }

#if LOG_CONSOLE
    fputs(line, stderr);
#endif
}

static void *log_thread(void *arg)
{
    (void)arg;

    while (1) {
        char line[LOG_LINE_MAX];

        pthread_mutex_lock(&s_lock);
        while (s_count == 0 && !s_quit)
            pthread_cond_wait(&s_cond, &s_lock);

        if (s_count == 0 && s_quit) {
            pthread_mutex_unlock(&s_lock);
            break;
        }

        memcpy(line, s_queue[s_head], sizeof(line));
        s_head = (s_head + 1) % LOG_QUEUE_LINES;
        s_count--;
        pthread_mutex_unlock(&s_lock);

        log_write_file(line);
    }
    return NULL;
}

int log_init(const char *dir)
{
    if (s_inited)
        return 0;

    const char *env_dir = getenv("GATEWAY_LOG_DIR");
    const char *env_lv = getenv("GATEWAY_LOG_LEVEL");
    const char *cands[5];
    int n = 0;

    if (dir && dir[0])
        cands[n++] = dir;
    if (env_dir && env_dir[0])
        cands[n++] = env_dir;
    cands[n++] = "/opt/edge-gateway/logs";
    cands[n++] = "/var/log/edge-gateway";
    cands[n++] = "./logs";

    s_level = level_parse(env_lv);

    s_dir[0] = 0;
    for (int i = 0; i < n; i++) {
        if (mkdir_p(cands[i], 0700) == 0 && access(cands[i], W_OK) == 0) {
            strncpy(s_dir, cands[i], sizeof(s_dir) - 1);
            s_dir[sizeof(s_dir) - 1] = 0;
            break;
        }
    }

    if (!s_dir[0]) {
        fprintf(stderr, "[LOG] no writable dir, fallback to stderr\n");
        s_inited = 0;
        return -1;
    }

    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/gateway.log", s_dir);

    s_fp = fopen(path, "a");
    if (!s_fp) {
        fprintf(stderr, "[LOG] open %s failed: %s\n", path, strerror(errno));
        s_inited = 0;
        return -1;
    }

    /* 日志可能包含事件/抓拍路径等运行信息，收紧为仅 root 可读写 */
    chmod(path, 0600);

    struct stat st;
    if (stat(path, &st) == 0)
        s_bytes = (long)st.st_size;
    else
        s_bytes = 0;

    s_head = s_tail = s_count = 0;
    s_dropped = 0;
    s_quit = 0;
    s_inited = 1;

    if (thread_start(&s_tid, 128 * 1024, log_thread, NULL) != 0) {
        fprintf(stderr, "[LOG] thread create failed, sync mode\n");
        s_inited = 0;
        fclose(s_fp);
        s_fp = NULL;
        return -1;
    }

    fprintf(stderr, "[LOG] file=%s level=%s max=%dKB x%d\n",
            path, level_name(s_level), LOG_DEFAULT_MAX_KB, LOG_DEFAULT_MAX_FILES);
    return 0;
}

void log_close(void)
{
    if (!s_inited)
        return;

    pthread_mutex_lock(&s_lock);
    s_quit = 1;
    pthread_cond_broadcast(&s_cond);
    pthread_mutex_unlock(&s_lock);

    pthread_join(s_tid, NULL);

    if (s_fp) {
        fflush(s_fp);
        fclose(s_fp);
        s_fp = NULL;
    }

    fprintf(stderr, "[LOG] closed (dropped=%lu)\n", s_dropped);
    s_inited = 0;
}

void log_set_level(log_level_t lv)
{
    if (lv < LOG_DEBUG) lv = LOG_DEBUG;
    if (lv > LOG_FATAL) lv = LOG_FATAL;
    s_level = lv;
}

log_level_t log_get_level(void)
{
    return (log_level_t)s_level;
}

void log_write(log_level_t lv, const char *module, const char *fmt, ...)
{
    if ((int)lv < s_level)
        return;

    char msg[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    localtime_r(&ts.tv_sec, &tm);

    char line[LOG_LINE_MAX];
    snprintf(line, sizeof(line),
             "%04d-%02d-%02d %02d:%02d:%02d.%03ld [%s] [%s] (tid=%lu) %s\n",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec,
             ts.tv_nsec / 1000000,
             level_name(lv),
             module ? module : "-",
             (unsigned long)pthread_self(),
             msg);

    if (!s_inited) {
        fputs(line, stderr);
        return;
    }

    pthread_mutex_lock(&s_lock);
    if (s_count >= LOG_QUEUE_LINES) {
        s_dropped++;
        pthread_mutex_unlock(&s_lock);
        return;
    }

    memcpy(s_queue[s_tail], line, sizeof(s_queue[s_tail]));
    s_tail = (s_tail + 1) % LOG_QUEUE_LINES;
    s_count++;
    pthread_cond_signal(&s_cond);
    pthread_mutex_unlock(&s_lock);
}

const char *log_dir(void)
{
    return s_dir;
}

int log_file_path(char *out, int n)
{
    if (!out || n <= 0 || !s_dir[0])
        return -1;
    snprintf(out, (size_t)n, "%s/gateway.log", s_dir);
    return 0;
}
