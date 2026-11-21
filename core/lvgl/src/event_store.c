/**
 * event_store.c — 本地事件缓存实现（T6）
 */
#include "event_store.h"
#include "log_svc.h"
#include "thread_util.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define EVENT_LINE_MAX 512

static char s_dir[PATH_MAX];
static char s_path[PATH_MAX];
static char s_lines[EVENT_STORE_MAX][EVENT_LINE_MAX];
static int s_count = 0;
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_cond = PTHREAD_COND_INITIALIZER;
static pthread_t s_writer;
static int s_writer_started = 0;
static volatile int s_quit = 0;
static int s_dirty = 0;
static uint32_t s_gen = 0;

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

static int fsync_dir(const char *path)
{
    char dir[PATH_MAX];
    strncpy(dir, path, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = 0;

    char *slash = strrchr(dir, '/');
    if (!slash)
        return 0;
    if (slash == dir)
        slash[1] = 0;
    else
        *slash = 0;

    int fd = open(dir, O_RDONLY | O_DIRECTORY);
    if (fd < 0)
        return -1;
    fsync(fd);
    close(fd);
    return 0;
}

/* 写入快照；不持锁（writer 线程调用）。
 * snap 为连续 count 行，每行 EVENT_LINE_MAX 字节。 */
static int es_write_snapshot(const char *snap, int count)
{
    if (!s_path[0])
        return -1;

    char tmp[PATH_MAX];
    if (snprintf(tmp, sizeof(tmp), "%s.tmp", s_path) >= (int)sizeof(tmp))
        return -1;

    FILE *fp = fopen(tmp, "w");
    if (!fp) {
        LOGE("EVENT", "open %s failed: %s", tmp, strerror(errno));
        return -1;
    }

    for (int i = 0; i < count; i++) {
        fputs(snap + (size_t)i * EVENT_LINE_MAX, fp);
        fputc('\n', fp);
    }

    fflush(fp);
    fsync(fileno(fp));
    fclose(fp);

    if (rename(tmp, s_path) != 0) {
        unlink(tmp);
        LOGE("EVENT", "rename %s failed: %s", s_path, strerror(errno));
        return -1;
    }

    /* 事件可能包含区域/时间等信息，收紧为仅 root 可读写 */
    chmod(s_path, 0600);
    fsync_dir(s_path);
    return 0;
}

/* 独立落盘线程：UI 线程只改内存，不阻塞触摸 */
static void *es_writer_thread(void *arg)
{
    (void)arg;

    while (1) {
        pthread_mutex_lock(&s_lock);
        while (!s_dirty && !s_quit)
            pthread_cond_wait(&s_cond, &s_lock);

        if (s_quit && !s_dirty) {
            pthread_mutex_unlock(&s_lock);
            break;
        }

        int count = s_count;
        uint32_t gen = s_gen;
        s_dirty = 0;

        char *snap = malloc((size_t)count * EVENT_LINE_MAX + 1);
        if (snap && count > 0)
            memcpy(snap, s_lines, (size_t)count * EVENT_LINE_MAX);
        pthread_mutex_unlock(&s_lock);

        if (snap)
            es_write_snapshot(snap, count);
        free(snap);

        pthread_mutex_lock(&s_lock);
        if (s_gen != gen)
            s_dirty = 1;      /* 写盘期间又有新变更，下一轮再写 */
        pthread_mutex_unlock(&s_lock);
    }
    return NULL;
}

/* 从 JSON 行提取 "id":"..."（不依赖完整 JSON 解析） */
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

static int es_find_locked(const char *id)
{
    char needle[80];
    snprintf(needle, sizeof(needle), "\"id\":\"%s\"", id);

    for (int i = 0; i < s_count; i++) {
        if (strstr(s_lines[i], needle))
            return i;
    }
    return -1;
}

int event_store_init(const char *dir)
{
    const char *env = getenv("GATEWAY_QUEUE_DIR");
    const char *cands[4];
    int n = 0;

    if (dir && dir[0])
        cands[n++] = dir;
    if (env && env[0])
        cands[n++] = env;
    cands[n++] = "/opt/edge-gateway/data/queue";
    cands[n++] = "./data/queue";

    pthread_mutex_lock(&s_lock);
    s_dir[0] = 0;
    s_path[0] = 0;
    s_count = 0;

    for (int i = 0; i < n; i++) {
        if (mkdir_p(cands[i], 0700) == 0 && access(cands[i], W_OK) == 0) {
            strncpy(s_dir, cands[i], sizeof(s_dir) - 1);
            s_dir[sizeof(s_dir) - 1] = 0;
            break;
        }
    }

    if (!s_dir[0]) {
        pthread_mutex_unlock(&s_lock);
        LOGW("EVENT", "no writable queue dir");
        return -1;
    }

    snprintf(s_path, sizeof(s_path), "%s/events.jsonl", s_dir);

    FILE *fp = fopen(s_path, "r");
    if (fp) {
        char line[EVENT_LINE_MAX];
        while (fgets(line, sizeof(line), fp)) {
            size_t len = strlen(line);
            while (len && (line[len - 1] == '\n' || line[len - 1] == '\r'))
                line[--len] = 0;
            if (len == 0)
                continue;

            if (s_count >= EVENT_STORE_MAX) {
                memmove(s_lines[0], s_lines[1],
                        sizeof(s_lines[0]) * (EVENT_STORE_MAX - 1));
                s_count--;
            }
            strncpy(s_lines[s_count], line, EVENT_LINE_MAX - 1);
            s_lines[s_count][EVENT_LINE_MAX - 1] = 0;
            s_count++;
        }
        fclose(fp);
    }

    s_quit = 0;
    s_dirty = 0;
    s_gen = 0;
    pthread_mutex_unlock(&s_lock);

    if (s_path[0]) {
        if (thread_start(&s_writer, 128 * 1024, es_writer_thread, NULL) == 0)
            s_writer_started = 1;
        else
            LOGW("EVENT", "writer thread create failed, sync fallback");
    }

    LOGI("EVENT", "queue %s, pending=%d", s_path, s_count);
    return 0;
}

int event_store_append(const char *json_line)
{
    if (!json_line || !json_line[0])
        return -1;

    char id[64];
    extract_id(json_line, id, sizeof(id));

    pthread_mutex_lock(&s_lock);
    if (!s_path[0]) {
        pthread_mutex_unlock(&s_lock);
        return -1;
    }

    if (id[0] && es_find_locked(id) >= 0) {
        /* 已存在（重试/重复上报），不重复写 */
        pthread_mutex_unlock(&s_lock);
        return 0;
    }

    if (s_count >= EVENT_STORE_MAX) {
        memmove(s_lines[0], s_lines[1],
                sizeof(s_lines[0]) * (EVENT_STORE_MAX - 1));
        s_count--;
    }

    strncpy(s_lines[s_count], json_line, EVENT_LINE_MAX - 1);
    s_lines[s_count][EVENT_LINE_MAX - 1] = 0;
    s_count++;

    s_dirty = 1;
    s_gen++;
    if (s_writer_started)
        pthread_cond_signal(&s_cond);
    else
        es_write_snapshot((const char *)s_lines, s_count);

    pthread_mutex_unlock(&s_lock);
    return 0;
}

int event_store_remove(const char *id)
{
    if (!id || !id[0])
        return -1;

    pthread_mutex_lock(&s_lock);
    int idx = es_find_locked(id);
    if (idx >= 0) {
        memmove(s_lines[idx], s_lines[idx + 1],
                sizeof(s_lines[0]) * (s_count - idx - 1));
        s_count--;

        s_dirty = 1;
        s_gen++;
        if (s_writer_started)
            pthread_cond_signal(&s_cond);
        else
            es_write_snapshot((const char *)s_lines, s_count);
    }
    pthread_mutex_unlock(&s_lock);
    return 0;
}

int event_store_count(void)
{
    pthread_mutex_lock(&s_lock);
    int n = s_count;
    pthread_mutex_unlock(&s_lock);
    return n;
}

int event_store_get(int idx, char *out, int n)
{
    if (!out || n <= 0)
        return -1;

    pthread_mutex_lock(&s_lock);
    if (idx < 0 || idx >= s_count) {
        pthread_mutex_unlock(&s_lock);
        return -1;
    }
    strncpy(out, s_lines[idx], (size_t)n - 1);
    out[n - 1] = 0;
    pthread_mutex_unlock(&s_lock);
    return 0;
}

const char *event_store_path(void)
{
    return s_path;
}

void event_store_close(void)
{
    if (!s_writer_started)
        return;

    pthread_mutex_lock(&s_lock);
    s_quit = 1;
    pthread_cond_broadcast(&s_cond);
    pthread_mutex_unlock(&s_lock);

    pthread_join(s_writer, NULL);
    s_writer_started = 0;
}
