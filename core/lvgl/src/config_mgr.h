/**
 * config_mgr.h — 应用配置持久化管理（JSON + 原子写）
 *
 * 设计目标（v2 T1）：
 *   - 把 UI 设置、预警阈值、检测参数统一落盘，重启不丢；
 *   - 掉电不损坏：临时文件 + fsync + rename + 目录 fsync；
 *   - 解析失败/字段越界时回退默认值，并在日志中记录；
 *   - 不引入第三方 JSON 库：内置最小 JSON 解析器（对象/数组/字符串/整数/布尔）。
 *
 * 线程模型：
 *   - config_update() 只改内存并置 dirty，不写盘；
 *   - config_flush() 由 UI 主线程的 1s 定时器调用，批量原子落盘；
 *   - config_get() 返回只读快照，调用方不要跨线程长期持有。
 *
 * 文件位置（config_init(NULL) 时按顺序探测）：
 *   1) 环境变量 GATEWAY_CONFIG
 *   2) /opt/edge-gateway/config/gateway.json
 *   3) /etc/edge-gateway/gateway.json
 *   4) ./gateway.json
 *   选择第一个目录可写的路径；写盘失败仅打印错误，不阻塞业务。
 */
#ifndef CONFIG_MGR_H
#define CONFIG_MGR_H

#include "config_types.h"   /* struct zone_warn_cfg / dev_config（中立类型） */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CONFIG_VERSION 2

/* 应用配置全量快照（v2 第一批只覆盖现有 UI 可调项） */
struct app_config {
    int version;

    /* 识别/检测 */
    int cloud_review;       /* 1=云端复核开 0=纯本地 */
    int motion_interval;    /* 帧差间隔 1~30 */
    int motion_diff;        /* 灰度差阈值 5~100 */
    int camera_zone;        /* 单摄固定归属防区 0~3（告警来源） */
    int ap_ps_off;          /* AP3216C 距离本底偏移 */
    int ap_ir_off;          /* AP3216C 红外本底偏移 */
    int ap_als_scale;       /* AP3216C 光照比例 ×1000 */

    /* 显示/交互 */
    int brightness;         /* 背光 0~100 */
    int schedule_on;        /* 定时布防 0/1 */
    int sched_start_h;      /* 0~23 */
    int sched_end_h;        /* 0~23 */

    /* 传感器校准（T3-C） */
    int calib_valid;        /* 1=校准参数有效 */
    int imu_off[3];         /* 加速度原始值零偏（ax/ay/az） */
    int imu_scale[3];       /* 比例 ×1000（1000=1.0） */
    int temp_off;           /* 温度原始值偏置 */
    /* C3 温漂拟合（纯自动：至少 2 个不同温度点才得到斜率） */
    int temp_k[3];          /* 每轴温漂斜率 LSB/℃ ×1000；0=未拟合 */
    int temp_ref;           /* 补偿参考温度（原始值） */
    int calib_pt_n;         /* 已记录温度点数量 0~4 */
    int calib_pt_temp[4];   /* 各温度点温度（原始值） */
    int calib_pt_off[3][4]; /* [轴][点] 静态基线零偏（原始值） */

    /* 传感器预警（复用 dev_bridge 的结构，保证两处字段一致） */
    struct dev_config warn;

    /* 网络（T7/T11）：MQTT broker 地址，持久化后无需每次带环境变量 */
    char mqtt_broker[64];
    char mqtt_user[48];      /* T11：为空=匿名 */
    char mqtt_pass[96];
    char cloud_api_key[80];  /* T11-B2：HTTP 接口鉴权（精判/OTA/白名单） */

    /* T11-③：MQTT 传输（默认 1883 明文，可选 8883 TLS） */
    int mqtt_port;           /* 1883 / 8883 */
    int mqtt_tls;            /* 1=启用 TLS（二进制需编译进 mbedTLS） */
    char mqtt_ca_path[128];  /* CA 证书 PEM 路径 */
};

/**
 * 初始化配置模块并加载配置文件。
 * @param path 指定路径；NULL 时按候选路径自动探测
 * @return 0=加载成功（或文件不存在但已用默认值）；-1=路径均不可写
 * @note 文件不存在/解析失败时使用默认值，并置 dirty，等待首次 flush 落盘。
 */
int config_init(const char *path);

/** 当前实际使用的配置文件路径（只读） */
const char *config_path(void);

/** 只读配置快照；不要修改返回值，也不要跨线程长期持有 */
const struct app_config *config_get(void);

/**
 * 更新内存配置并置 dirty（不写盘）。
 * 内部会做范围裁剪；失败（如 cfg=NULL）返回 -1。
 */
int config_update(const struct app_config *cfg);

/** 若 dirty 则原子写盘；成功返回 0，失败返回 -1（保留 dirty 以便重试） */
int config_flush(void);

/** 是否有未落盘的修改 */
int config_is_dirty(void);

/**
 * 配置代数：每次 config_update 自增（T11-B3 设备影子用）
 * 调用方可据此判断“配置是否变化”，避免轮询比对全量字段。
 */
uint32_t config_generation(void);

/** 填充默认配置（不访问文件） */
void config_defaults(struct app_config *out);

#ifdef __cplusplus
}
#endif

#endif /* CONFIG_MGR_H */
