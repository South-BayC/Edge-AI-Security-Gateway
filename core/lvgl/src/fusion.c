/**
 * fusion.c — 多源事件融合实现（C1/C5）
 */
#include "fusion.h"

#include <string.h>

#define FUSE_ZONES   4
#define FUSE_MAX_EV  8

typedef struct {
    uint32_t ts[FUSE_MAX_EV];
    int      src[FUSE_MAX_EV];
    int      strength[FUSE_MAX_EV];
    int      n;
    int      idx;
} fuse_zone_t;

static fuse_zone_t s_z[FUSE_ZONES];

void fusion_reset(void)
{
    memset(s_z, 0, sizeof(s_z));
}

void fusion_add(int zone, int src, uint32_t now_ms, int strength)
{
    if (zone < 0 || zone >= FUSE_ZONES)
        return;

    fuse_zone_t *z = &s_z[zone];
    z->ts[z->idx] = now_ms;
    z->src[z->idx] = src;
    z->strength[z->idx] = strength < 0 ? 0 : (strength > 100 ? 100 : strength);
    z->idx = (z->idx + 1) % FUSE_MAX_EV;
    if (z->n < FUSE_MAX_EV)
        z->n++;
}

static int zone_score(const fuse_zone_t *z, uint32_t now, uint32_t win)
{
    int cam = 0, sen = 0;

    for (int i = 0; i < z->n; i++) {
        if ((uint32_t)(now - z->ts[i]) > win)
            continue;
        if (z->src[i] == FUSE_SRC_CAM) {
            if (z->strength[i] > cam) cam = z->strength[i];
        } else if (z->src[i] == FUSE_SRC_SENSOR) {
            if (z->strength[i] > sen) sen = z->strength[i];
        }
    }

    if (!cam && !sen)
        return 0;

    int base = cam > sen ? cam : sen;
    int sc = base;

    if (cam && sen) {
        sc = base + 25;            /* 双源共现加分 */
    } else if (sc > 60) {
        sc = 60;                   /* 单源封顶：低于默认阈值 70 */
    }

    return sc > 100 ? 100 : sc;
}

int fusion_score(int zone, uint32_t now_ms, uint32_t win_ms)
{
    if (zone < 0 || zone >= FUSE_ZONES)
        return 0;
    return zone_score(&s_z[zone], now_ms, win_ms);
}

int fusion_poll(uint32_t now_ms, uint32_t win_ms, int threshold, int *score_out)
{
    for (int i = 0; i < FUSE_ZONES; i++) {
        fuse_zone_t *z = &s_z[i];
        if (z->n == 0)
            continue;

        /* 以“最后一个事件”为窗口右端计算分数：
         * 窗口过期后事件已落在 now 的窗口外，若按 now 计算会得到 0，
         * 无法反映刚才累积的置信度。 */
        uint32_t last = z->ts[0];
        for (int k = 1; k < z->n; k++) {
            if ((int32_t)(z->ts[k] - last) > 0)
                last = z->ts[k];
        }

        int sc = zone_score(z, last, win_ms);
        int expired = ((uint32_t)(now_ms - last) > win_ms);

        /* 达到阈值立即定案；窗口过期也定案 */
        if (sc >= threshold || expired) {
            if (score_out)
                *score_out = sc;
            z->n = 0;
            z->idx = 0;
            return i;
        }
    }
    return -1;
}
