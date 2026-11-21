/**
 * fusion.h — 多源事件融合 + 置信度评分（C1/C5）
 *
 * 思路：
 *   - 每个分区维护最近 8 个事件（来源 + 时间戳 + 强度）；
 *   - 时间窗内（默认 3s）同时出现摄像头与传感器事件 → 置信度 +25（融合加分）；
 *   - 只有单一来源时置信度封顶 60，低于默认阈值 70，只做轻提醒；
 *   - 达到阈值立即定案；窗口过期且未达阈值 → 单源事件。
 *
 * 默认关闭（`dev_config.fusion_on=0`），保持原有即时告警语义。
 */
#ifndef FUSION_H
#define FUSION_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FUSE_SRC_CAM     1   /* 摄像头运动/精判 */
#define FUSE_SRC_SENSOR  2   /* 传感器阈值 */

/** 清空所有分区状态（测试/重启用） */
void fusion_reset(void);

/**
 * 记录一个事件
 * @param zone     分区 0-3
 * @param src      FUSE_SRC_*
 * @param now_ms   单调时钟毫秒
 * @param strength 单源强度 0-100
 */
void fusion_add(int zone, int src, uint32_t now_ms, int strength);

/** 计算窗口内置信度 0-100（不消费事件） */
int fusion_score(int zone, uint32_t now_ms, uint32_t win_ms);

/**
 * 取一个待定案分区（读后清空该分区）
 * @param now_ms     当前单调毫秒
 * @param win_ms     时间窗
 * @param threshold  融合阈值（达到即定案）
 * @param score_out  输出该分区置信度
 * @return 分区号 0-3；-1=暂无定案
 */
int fusion_poll(uint32_t now_ms, uint32_t win_ms, int threshold, int *score_out);

#ifdef __cplusplus
}
#endif

#endif
