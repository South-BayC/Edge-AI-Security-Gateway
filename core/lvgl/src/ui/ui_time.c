/**
 * ui_time.c — UI 时间字符串实现
 */
#include "ui_time.h"

#include <stdio.h>

static int s_day_sec = 0;

void ui_time_set_day_seconds(int sec)
{
    s_day_sec = sec;
}

void ui_time_str(char *buf, int len)
{
    if (!buf || len <= 0) return;
    int total = s_day_sec % 86400;
    snprintf(buf, len, "%02d:%02d", total / 3600, (total % 3600) / 60);
}
