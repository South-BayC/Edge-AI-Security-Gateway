/**
 * test_warn_policy.c — 告警分级声光策略测试
 *
 * 说明：本测试锁定「重构前」的实际行为（含既有节拍细节），
 * 重构只允许搬运逻辑，不允许顺手改时序。
 */
#include <stdio.h>
#include "domain/alarm/warn_policy.h"

static int fails;

#define CHECK(cond, name) do { \
    if (!(cond)) { printf("FAIL: %s\n", name); fails++; } \
    else { printf("ok  : %s\n", name); } \
} while (0)

int main(void)
{
    warn_policy_t w;

    /* 1. 无告警：LED 跟随布防 */
    warn_policy_reset(&w);
    warn_policy_step(&w, 0, 1);
    CHECK(warn_policy_led(&w) == 1 && warn_policy_beep(&w) == 0, "idle armed -> led on");
    warn_policy_step(&w, 0, 0);
    CHECK(warn_policy_led(&w) == 0, "idle disarmed -> led off");

    /* 2. 高等级（3）：LED 常亮（period=1）+ 蜂鸣器持续长鸣 */
    warn_policy_reset(&w);
    int all_on = 1;
    for (int i = 0; i < 6; i++) {
        warn_policy_step(&w, 3, 1);
        if (warn_policy_led(&w) != 1 || warn_policy_beep(&w) != 1)
            all_on = 0;
    }
    CHECK(all_on, "level 3 -> led steady + beep continuous");

    /* 3. 中等级（2）：蜂鸣器 2 拍鸣/2 拍停（400ms/400ms） */
    warn_policy_reset(&w);
    warn_policy_step(&w, 2, 1);
    CHECK(warn_policy_beep(&w) == 0, "level 2 beep phase1 off");
    warn_policy_step(&w, 2, 1);
    CHECK(warn_policy_beep(&w) == 1, "level 2 beep phase2 on");
    warn_policy_step(&w, 2, 1);
    CHECK(warn_policy_beep(&w) == 1, "level 2 beep phase3 on");
    warn_policy_step(&w, 2, 1);
    CHECK(warn_policy_beep(&w) == 0, "level 2 beep phase4 off");

    /* 4. 低等级（1）：4 拍内 1 拍短鸣 */
    warn_policy_reset(&w);
    int on_count = 0;
    for (int i = 0; i < 4; i++) {
        warn_policy_step(&w, 1, 1);
        on_count += warn_policy_beep(&w);
    }
    CHECK(on_count == 1, "level 1 -> one short beep per 4 ticks");

    /* 5. 静音：告警期间不响；消警后自动解除静音 */
    warn_policy_reset(&w);
    warn_policy_set_mute(&w, 1);
    warn_policy_step(&w, 3, 1);
    CHECK(warn_policy_beep(&w) == 0, "muted during alarm -> beep off");
    warn_policy_step(&w, 0, 1);
    CHECK(w.muted == 0, "mute auto-cleared after alarm cleared");
    CHECK(warn_policy_led(&w) == 1, "led still follows armed while mute cleared");

    /* 6. 手动锁定优先（无告警时），告警到来时让位 */
    warn_policy_reset(&w);
    warn_policy_set_led_manual(&w, 1);
    warn_policy_set_beep_manual(&w, 1);
    warn_policy_step(&w, 0, 0);
    CHECK(warn_policy_led(&w) == 1 && warn_policy_beep(&w) == 1, "manual lock holds when idle");
    warn_policy_step(&w, 2, 0);
    CHECK(w.led_manual == -1 && w.beep_manual == -1, "alarm clears manual lock");
    warn_policy_step(&w, 0, 0);
    CHECK(warn_policy_led(&w) == 0 && warn_policy_beep(&w) == 0, "after alarm -> auto off, not manual");

    printf("%s\n", fails ? "RESULT: FAIL" : "RESULT: PASS");
    return fails ? 1 : 0;
}
