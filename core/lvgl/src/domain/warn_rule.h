/**
 * warn_rule.h — 传感器阈值判定（双阈值 + 防抖），domain 层纯逻辑
 *
 * 原为 dev_bridge 采集线程内的两段重复代码（AP3216C / ICM20608 各一份），
 * 抽出后统一：命中告警阈值 → HIT；仅命中预警阈值 → PENDING；
 * 连续 debounce 次才上报，上报后计数归零（便于冷却结束后再次触发）。
 *
 * 依赖：config_types.h（zone_warn_cfg / WARN_CMP_*）。无硬件、无 UI。
 */
#ifndef WARN_RULE_H
#define WARN_RULE_H

#include "../config_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WARN_RULE_NONE = 0,   /* 无事件 */
    WARN_RULE_PENDING,    /* 刚达到预警阈值（只轻提醒） */
    WARN_RULE_HIT         /* 刚达到告警阈值 */
} warn_rule_evt_t;

typedef struct {
    int cnt[4];        /* 各分区告警去抖计数 */
    int cnt_warn[4];   /* 各分区预警去抖计数 */
} warn_rule_t;

void warn_rule_reset(warn_rule_t *r);

/** 阈值比较：GT / EQ / LT */
int warn_rule_match(long long val, int threshold, int cmp);

/**
 * 评估一个分区。
 * @param r        去抖状态
 * @param z        分区索引 0~3
 * @param c        分区阈值配置
 * @param val      当前数值（已滤波/校准）
 * @param debounce 连续命中次数（<=0 时每次命中即上报，与旧实现一致）
 * @return 本拍产生的事件
 */
warn_rule_evt_t warn_rule_eval(warn_rule_t *r, int z,
                               const struct zone_warn_cfg *c,
                               long long val, int debounce);

#ifdef __cplusplus
}
#endif

#endif /* WARN_RULE_H */
