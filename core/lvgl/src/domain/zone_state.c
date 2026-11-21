/**
 * zone_state.c — 安防分区状态实现（领域层）
 */
#include "zone_state.h"

static zone_state_t s_state[ZONE_COUNT];
static const char *const s_names[ZONE_COUNT] = { "前门", "后门", "窗户", "仓库" };

void zone_state_init(void)
{
    for (int i = 0; i < ZONE_COUNT; i++)
        s_state[i] = ZONE_ONLINE;
}

void zone_state_set(int id, zone_state_t st)
{
    if (id < 0 || id >= ZONE_COUNT)
        return;
    s_state[id] = st;
}

zone_state_t zone_state_get(int id)
{
    if (id < 0 || id >= ZONE_COUNT)
        return ZONE_ONLINE;
    return s_state[id];
}

int zone_state_is_armed(int id)
{
    return zone_state_get(id) == ZONE_ARMED;
}

int zone_state_any_armed(void)
{
    for (int i = 0; i < ZONE_COUNT; i++)
        if (s_state[i] == ZONE_ARMED)
            return 1;
    return 0;
}

int zone_state_has_alarm(int id)
{
    return zone_state_get(id) == ZONE_ALARM;
}

int zone_state_any_alarm(void)
{
    for (int i = 0; i < ZONE_COUNT; i++)
        if (s_state[i] == ZONE_ALARM)
            return 1;
    return 0;
}

int zone_state_alarm_level(void)
{
    static const int lvl[ZONE_COUNT] = { 1, 1, 3, 2 };
    int max = 0;
    for (int i = 0; i < ZONE_COUNT; i++)
        if (s_state[i] == ZONE_ALARM && lvl[i] > max)
            max = lvl[i];
    return max;
}

const char *zone_state_name(int id)
{
    if (id < 0 || id >= ZONE_COUNT)
        return "";
    return s_names[id];
}

void zone_state_arm_all(void)
{
    for (int i = 0; i < ZONE_COUNT; i++)
        if (s_state[i] != ZONE_ALARM)
            s_state[i] = ZONE_ARMED;
}

void zone_state_disarm_all(void)
{
    for (int i = 0; i < ZONE_COUNT; i++)
        s_state[i] = ZONE_ONLINE;
}

void zone_state_ack(int id)
{
    if (id < 0 || id >= ZONE_COUNT)
        return;
    if (s_state[id] == ZONE_ALARM)
        s_state[id] = ZONE_ARMED;
}
