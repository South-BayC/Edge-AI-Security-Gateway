/**
 * alarm_fsm.c — 分区告警状态机实现（T5）
 */
#include "alarm_fsm.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>

static alarm_state_t s_state[ALARM_ZONE_MAX];
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static alarm_fsm_cb s_cb = NULL;
static void *s_cb_arg = NULL;

void alarm_fsm_init(void)
{
    pthread_mutex_lock(&s_lock);
    for (int i = 0; i < ALARM_ZONE_MAX; i++)
        s_state[i] = ALARM_NORMAL;
    pthread_mutex_unlock(&s_lock);
}

void alarm_fsm_set_callback(alarm_fsm_cb cb, void *arg)
{
    pthread_mutex_lock(&s_lock);
    s_cb = cb;
    s_cb_arg = arg;
    pthread_mutex_unlock(&s_lock);
}

static alarm_state_t next_state(alarm_state_t old, alarm_event_t ev)
{
    alarm_state_t next = old;

    switch (old) {
    case ALARM_NORMAL:
        if (ev == ALARM_EV_TRIGGER)
            next = ALARM_PENDING;
        break;

    case ALARM_PENDING:
        if (ev == ALARM_EV_CONFIRM || ev == ALARM_EV_TRIGGER)
            next = ALARM_ACTIVE;
        else if (ev == ALARM_EV_ACK || ev == ALARM_EV_CLEAR ||
                 ev == ALARM_EV_TIMEOUT)
            next = ALARM_NORMAL;
        break;

    case ALARM_ACTIVE:
        if (ev == ALARM_EV_ACK || ev == ALARM_EV_MUTE)
            next = ALARM_MUTED;
        else if (ev == ALARM_EV_CLEAR)
            next = ALARM_RECOVERY;
        else if (ev == ALARM_EV_TRIGGER)
            next = ALARM_ACTIVE;
        break;

    case ALARM_MUTED:
        if (ev == ALARM_EV_CLEAR)
            next = ALARM_RECOVERY;
        else if (ev == ALARM_EV_TRIGGER)
            next = ALARM_ACTIVE;
        else if (ev == ALARM_EV_TIMEOUT)
            next = ALARM_NORMAL;
        break;

    case ALARM_RECOVERY:
        if (ev == ALARM_EV_CLEAR || ev == ALARM_EV_TIMEOUT)
            next = ALARM_NORMAL;
        else if (ev == ALARM_EV_TRIGGER)
            next = ALARM_PENDING;
        break;

    default:
        next = ALARM_NORMAL;
        break;
    }

    return next;
}

int alarm_fsm_dispatch(int zone, alarm_event_t ev, const char *reason)
{
    if (zone < 0 || zone >= ALARM_ZONE_MAX)
        return 0;

    alarm_state_t old_state, new_state;
    alarm_fsm_cb cb;
    void *arg;
    char rbuf[64];

    snprintf(rbuf, sizeof(rbuf), "%s", reason ? reason : "");

    pthread_mutex_lock(&s_lock);
    old_state = s_state[zone];
    new_state = next_state(old_state, ev);
    if (new_state == old_state) {
        pthread_mutex_unlock(&s_lock);
        return 0;
    }
    s_state[zone] = new_state;
    cb = s_cb;
    arg = s_cb_arg;
    pthread_mutex_unlock(&s_lock);

    if (cb)
        cb(zone, old_state, new_state, rbuf, arg);

    return 1;
}

alarm_state_t alarm_fsm_state(int zone)
{
    if (zone < 0 || zone >= ALARM_ZONE_MAX)
        return ALARM_NORMAL;

    pthread_mutex_lock(&s_lock);
    alarm_state_t st = s_state[zone];
    pthread_mutex_unlock(&s_lock);
    return st;
}

int alarm_fsm_alarming(int zone)
{
    alarm_state_t st = alarm_fsm_state(zone);
    return st == ALARM_PENDING || st == ALARM_ACTIVE || st == ALARM_MUTED;
}

int alarm_fsm_any_alarming(void)
{
    for (int i = 0; i < ALARM_ZONE_MAX; i++) {
        if (alarm_fsm_alarming(i))
            return 1;
    }
    return 0;
}

const char *alarm_state_name(alarm_state_t st)
{
    switch (st) {
    case ALARM_NORMAL:   return "NORMAL";
    case ALARM_PENDING:  return "PENDING";
    case ALARM_ACTIVE:   return "ACTIVE";
    case ALARM_MUTED:    return "MUTED";
    case ALARM_RECOVERY: return "RECOVERY";
    default:             return "?";
    }
}
