/**
 * event_bus.c — 统一事件总线实现（T5）
 */
#include "event_bus.h"

#include <pthread.h>
#include <string.h>
#include <time.h>

#define EVENT_BUS_MAX 64

static system_event_t s_q[EVENT_BUS_MAX];
static int s_count = 0;
static uint32_t s_next_id = 1;
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000L);
}

void event_bus_init(void)
{
    pthread_mutex_lock(&s_lock);
    memset(s_q, 0, sizeof(s_q));
    s_count = 0;
    s_next_id = 1;
    pthread_mutex_unlock(&s_lock);
}

int event_bus_publish(const system_event_t *ev)
{
    if (!ev)
        return -1;

    pthread_mutex_lock(&s_lock);

    system_event_t e = *ev;
    if (e.id == 0)
        e.id = s_next_id++;
    if (e.ts_ms == 0)
        e.ts_ms = now_ms();

    if (s_count < EVENT_BUS_MAX) {
        s_q[s_count++] = e;
        pthread_mutex_unlock(&s_lock);
        return 0;
    }

    /* 队列满：找最低优先级（同优先级取最旧）的事件 */
    int worst = 0;
    for (int i = 1; i < s_count; i++) {
        if (s_q[i].prio > s_q[worst].prio)
            worst = i;
    }

    if (e.prio < s_q[worst].prio) {
        s_q[worst] = e;
        pthread_mutex_unlock(&s_lock);
        return 0;
    }

    pthread_mutex_unlock(&s_lock);
    return -1;
}

int event_bus_poll(system_event_t *out)
{
    if (!out)
        return 0;

    pthread_mutex_lock(&s_lock);
    if (s_count == 0) {
        pthread_mutex_unlock(&s_lock);
        return 0;
    }

    /* 最高优先级（数值最小）；同优先级取最早入队 */
    int best = 0;
    for (int i = 1; i < s_count; i++) {
        if (s_q[i].prio < s_q[best].prio)
            best = i;
    }

    *out = s_q[best];
    memmove(&s_q[best], &s_q[best + 1],
            sizeof(system_event_t) * (size_t)(s_count - best - 1));
    s_count--;
    pthread_mutex_unlock(&s_lock);
    return 1;
}

int event_bus_pending(void)
{
    pthread_mutex_lock(&s_lock);
    int n = s_count;
    pthread_mutex_unlock(&s_lock);
    return n;
}
