/**
 * warn_policy.c — 告警分级声光策略实现
 */
#include "warn_policy.h"

void warn_policy_reset(warn_policy_t *w)
{
    if (!w)
        return;
    w->led_phase = 0;
    w->snd_phase = 0;
    w->led_manual = -1;
    w->beep_manual = -1;
    w->muted = 0;
    w->led = 0;
    w->beep = 0;
}

void warn_policy_step(warn_policy_t *w, int level, int armed)
{
    if (!w)
        return;

    if (level > 0) {
        /* 告警优先：清除手动锁定 */
        w->led_manual = -1;
        w->led_phase++;
        int period = (level >= 3) ? 1 : (level == 2 ? 2 : 4);
        w->led = (w->led_phase / 2) % period ? 0 : 1;   /* 按级别周期翻转 */

        if (w->muted) {
            w->beep = 0;
        } else {
            w->beep_manual = -1;
            w->snd_phase++;
            w->beep = 1;                                        /* 高：持续长鸣 */
            if (level == 2) w->beep = (w->snd_phase / 2) % 2;    /* 中：400ms 鸣/停 */
            if (level == 1) w->beep = (w->snd_phase % 4) == 0;   /* 低：200ms 短鸣/600ms 停 */
        }
        return;
    }

    /* 无告警 */
    if (w->muted)
        w->muted = 0;    /* 告警解除，自动恢复跟随 */

    w->led = (w->led_manual >= 0) ? w->led_manual : (armed ? 1 : 0);
    w->beep = (w->beep_manual >= 0) ? w->beep_manual : 0;
}

void warn_policy_set_led_manual(warn_policy_t *w, int on)
{
    if (!w)
        return;
    w->led_manual = on ? 1 : 0;
}

void warn_policy_set_beep_manual(warn_policy_t *w, int on)
{
    if (!w)
        return;
    w->beep_manual = on ? 1 : 0;
}

void warn_policy_set_mute(warn_policy_t *w, int mute)
{
    if (!w)
        return;
    w->muted = mute ? 1 : 0;
    if (w->muted)
        w->beep = 0;   /* 立即静音 */
}

int warn_policy_led(const warn_policy_t *w)
{
    return w ? w->led : 0;
}

int warn_policy_beep(const warn_policy_t *w)
{
    return w ? w->beep : 0;
}
