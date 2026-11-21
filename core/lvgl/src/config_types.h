/**
 * config_types.h — 配置层中立数据类型
 *
 * 目的：把「安防位置预警配置」从硬件桥接层（dev_bridge.h）中抽出，
 * 使 config_mgr / 远端配置 / 单元测试不依赖任何硬件或 UI 头文件。
 *
 * 依赖规则：本文件只允许依赖 C 标准库，不允许包含 lvgl.h、dev_bridge.h、UI 头文件。
 */
#ifndef CONFIG_TYPES_H
#define CONFIG_TYPES_H

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 预警触发源 ---- */
#define WARN_SRC_NONE    0   /* 不触发 */
#define WARN_SRC_AP3216C 1
#define WARN_SRC_ICM     2

/* AP3216C 数据通道（src = WARN_SRC_AP3216C） */
#define AP_DATA_PS       0   /* 接近度（越大越近） */
#define AP_DATA_ALS      1   /* 光照 */
#define AP_DATA_IR       2   /* 红外 */

/* ICM20608 数据通道（src = WARN_SRC_ICM）
 * MAG/LIN = 去重力线性加速度（静止≈0），阈值不受安装角度影响。 */
#define IMU_DATA_MAG     0
#define IMU_DATA_LIN     IMU_DATA_MAG
#define IMU_DATA_AX     1
#define IMU_DATA_AY     2
#define IMU_DATA_AZ     3
#define IMU_DATA_TEMP   4

/* 比较方式 */
#define WARN_CMP_GT      0   /* 数据 > 阈值 触发 */
#define WARN_CMP_EQ      1   /* 数据 == 阈值 触发 */
#define WARN_CMP_LT      2   /* 数据 < 阈值 触发 */

/** 单个安防位置的预警配置（0=前门 1=后门 2=窗户 3=仓库） */
struct zone_warn_cfg {
    int src;            /* WARN_SRC_* */
    int channel;        /* AP_DATA_* / IMU_DATA_* */
    int threshold;      /* 告警阈值 */
    int threshold_warn; /* 预警阈值（0=关闭） */
    int cmp;            /* WARN_CMP_* */
    int enabled;
};

/** 预警/融合配置（持久化在 config_mgr） */
struct dev_config {
    struct zone_warn_cfg zones[4];
    int debounce;            /* 连续 N 次超限才触发 */
    int cooldown_sec;        /* 消警后冷却秒数 */
    int fusion_on;           /* 1=启用多源融合 */
    int fusion_threshold;    /* 融合置信度阈值 0-100 */
    int fusion_window_ms;    /* 融合时间窗 */
};

#ifdef __cplusplus
}
#endif

#endif /* CONFIG_TYPES_H */
