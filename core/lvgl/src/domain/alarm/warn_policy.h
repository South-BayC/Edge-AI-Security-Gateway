/**
 * warn_policy.h — 告警分级声光策略（领域层，纯逻辑）
 *
 * 输入：告警级别 0~3、是否布防、每 200ms 一拍；
 * 输出：LED / 蜂鸣器期望状态（由调用方写入 infra/hal_io）。
 *
 * 规则（设计 2.2）：
 *   3=高（窗/周界） LED 每拍翻 + 持续长鸣
 *   2=中（仓库）   LED 2 拍翻 + 400ms 鸣/停
 *   1=低（门）     LED 4 拍翻 + 200ms 短鸣/600ms 停
 *   0=无告警       跟随布防（布防亮）/ 手动锁定 / 静音复位
 *
 * 无硬件、无 UI 依赖，可在 PC 上单测。
 */
#ifndef WARN_POLICY_H
#define WARN_POLICY_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int led_phase;    /* LED 翻转相位（每拍 +1） */
    int snd_phase;    /* 蜂鸣器节奏相位（每拍 +1） */
    int led_manual;   /* -1=自动跟随 0/1=手动锁定 */
    int beep_manual;  /* -1=自动跟随 0/1=手动锁定 */
    int muted;        /* 1=静音挂起（消警自动解除） */
    int led;          /* 当前 LED 输出 1/0 */
    int beep;         /* 当前蜂鸣器输出 1/0 */
} warn_policy_t;

void warn_policy_reset(warn_policy_t *w);

/** 推进一拍（调用周期 200ms） */
void warn_policy_step(warn_policy_t *w, int level, int armed);

/** 手动锁定 LED（-1 恢复自动） */
void warn_policy_set_led_manual(warn_policy_t *w, int on);
/** 手动锁定蜂鸣器（-1 恢复自动） */
void warn_policy_set_beep_manual(warn_policy_t *w, int on);
/** 静音挂起：1=立即停鸣；0=恢复跟随 */
void warn_policy_set_mute(warn_policy_t *w, int mute);

int warn_policy_led(const warn_policy_t *w);
int warn_policy_beep(const warn_policy_t *w);

#ifdef __cplusplus
}
#endif

#endif /* WARN_POLICY_H */
