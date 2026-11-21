/**
 * warn_rule.c — 传感器阈值判定实现
 */
#include "warn_rule.h"

#include <string.h>

void warn_rule_reset(warn_rule_t *r)
{
    if (!r)
        return;
    memset(r, 0, sizeof(*r));
}

int warn_rule_match(long long val, int threshold, int cmp)
{
    return (cmp == WARN_CMP_LT) ? (val < threshold) :
           (cmp == WARN_CMP_EQ) ? (val == threshold) :
           (val > threshold);
}

warn_rule_evt_t warn_rule_eval(warn_rule_t *r, int z,
                               const struct zone_warn_cfg *c,
                               long long val, int debounce)
{
    if (!r || !c || z < 0 || z > 3)
        return WARN_RULE_NONE;

    warn_rule_evt_t evt = WARN_RULE_NONE;
    int hit = warn_rule_match(val, c->threshold, c->cmp);

    /* C2 双阈值：未达告警阈值但超过预警阈值 → 只轻提醒 */
    if (!hit && c->threshold_warn > 0) {
        int wh = warn_rule_match(val, c->threshold_warn, c->cmp);
        if (wh)
            r->cnt_warn[z]++;
        else
            r->cnt_warn[z] = 0;

        if (r->cnt_warn[z] >= debounce) {
            r->cnt_warn[z] = 0;
            evt = WARN_RULE_PENDING;
        }
    } else {
        r->cnt_warn[z] = 0;
    }

    if (hit)
        r->cnt[z]++;
    else
        r->cnt[z] = 0;

    if (r->cnt[z] >= debounce) {
        /* 达到防抖次数：置位一次并重置计数，
         * 使数据持续超限时能周期性重新置位（冷却结束后立即可再次触发） */
        r->cnt[z] = 0;
        evt = WARN_RULE_HIT;
    }

    return evt;
}
