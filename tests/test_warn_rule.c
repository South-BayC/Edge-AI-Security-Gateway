/**
 * test_warn_rule.c — 双阈值 + 防抖判定测试
 */
#include <stdio.h>
#include "domain/warn_rule.h"

static int fails;

#define CHECK(cond, name) do { \
    if (!(cond)) { printf("FAIL: %s\n", name); fails++; } \
    else { printf("ok  : %s\n", name); } \
} while (0)

static struct zone_warn_cfg mk(int threshold, int warn, int cmp)
{
    struct zone_warn_cfg c;
    c.src = WARN_SRC_AP3216C;
    c.channel = AP_DATA_PS;
    c.threshold = threshold;
    c.threshold_warn = warn;
    c.cmp = cmp;
    c.enabled = 1;
    return c;
}

int main(void)
{
    warn_rule_t r;
    struct zone_warn_cfg c = mk(30, 20, WARN_CMP_GT);

    /* 1. 防抖：连续 2 次命中才上报 */
    warn_rule_reset(&r);
    CHECK(warn_rule_eval(&r, 0, &c, 31, 2) == WARN_RULE_NONE, "hit 1st -> none (debounce)");
    CHECK(warn_rule_eval(&r, 0, &c, 31, 2) == WARN_RULE_HIT, "hit 2nd -> HIT");
    CHECK(r.cnt[0] == 0, "counter reset after HIT");

    /* 2. 未达阈值：无事件、计数清零 */
    warn_rule_reset(&r);
    warn_rule_eval(&r, 0, &c, 10, 2);
    CHECK(warn_rule_eval(&r, 0, &c, 10, 2) == WARN_RULE_NONE, "below threshold -> none");

    /* 3. 双阈值：预警区间连续 2 次 → PENDING */
    warn_rule_reset(&r);
    CHECK(warn_rule_eval(&r, 0, &c, 25, 2) == WARN_RULE_NONE, "warn 1st -> none");
    CHECK(warn_rule_eval(&r, 0, &c, 25, 2) == WARN_RULE_PENDING, "warn 2nd -> PENDING");

    /* 4. 达标告警时清空预警计数（不会同时 PENDING） */
    warn_rule_reset(&r);
    warn_rule_eval(&r, 0, &c, 25, 2);
    warn_rule_eval(&r, 0, &c, 31, 2);
    CHECK(r.cnt_warn[0] == 0, "alarm clears warn counter");

    /* 5. 比较方向：LT（低于阈值触发） */
    struct zone_warn_cfg lt = mk(10, 0, WARN_CMP_LT);
    warn_rule_reset(&r);
    CHECK(warn_rule_eval(&r, 1, &lt, 9, 1) == WARN_RULE_HIT, "LT cmp triggers below");
    warn_rule_reset(&r);
    CHECK(warn_rule_eval(&r, 1, &lt, 11, 1) == WARN_RULE_NONE, "LT cmp ignores above");

    /* 6. 中间断开：计数清零，不误报 */
    warn_rule_reset(&r);
    warn_rule_eval(&r, 0, &c, 31, 2);
    warn_rule_eval(&r, 0, &c, 10, 2);
    CHECK(warn_rule_eval(&r, 0, &c, 31, 2) == WARN_RULE_NONE, "break resets debounce");

    /* 7. 持续超限：周期性再次上报 */
    warn_rule_reset(&r);
    warn_rule_eval(&r, 0, &c, 31, 2);
    CHECK(warn_rule_eval(&r, 0, &c, 31, 2) == WARN_RULE_HIT, "periodic HIT 1");
    warn_rule_eval(&r, 0, &c, 31, 2);
    CHECK(warn_rule_eval(&r, 0, &c, 31, 2) == WARN_RULE_HIT, "periodic HIT 2");

    /* 8. 关闭的分区不参与（由调用方过滤，这里只验证越界安全） */
    warn_rule_reset(&r);
    CHECK(warn_rule_eval(&r, -1, &c, 999, 1) == WARN_RULE_NONE, "negative zone safe");
    CHECK(warn_rule_eval(&r, 4, &c, 999, 1) == WARN_RULE_NONE, "zone 4 safe");

    printf("%s\n", fails ? "RESULT: FAIL" : "RESULT: PASS");
    return fails ? 1 : 0;
}
