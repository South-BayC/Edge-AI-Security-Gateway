/**
 * error_code.h — 统一错误码表（B4）
 *
 * 编码：负整数 -(模块号 << 8 | 错误号)
 *   - 返回值约定不变（0=成功，负数=失败），只是把“失败原因”标准化；
 *   - 日志中用 err_str() 打印可读名，便于全链路检索（如 grep "OTA:SHA"）。
 *
 * 模块号见 ERR_MOD_*；新增模块/错误时同步更新 docs/error-codes.md。
 */
#ifndef ERROR_CODE_H
#define ERROR_CODE_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ERR_MOD_OK      = 0,
    ERR_MOD_CFG     = 1,   /* config_mgr */
    ERR_MOD_LOG     = 2,   /* log_svc */
    ERR_MOD_SENSOR  = 3,   /* sensor_mgr */
    ERR_MOD_CAM     = 4,   /* cam_feed */
    ERR_MOD_DET     = 5,   /* detector */
    ERR_MOD_MQTT    = 6,   /* mqtt_hub */
    ERR_MOD_OTA     = 7,   /* ota_mgr */
    ERR_MOD_TIME    = 8,   /* time_sync */
    ERR_MOD_BRIDGE  = 9,   /* dev_bridge */
    ERR_MOD_HEALTH  = 10,  /* health_mgr */
    ERR_MOD_CLOUD   = 11,  /* 云端 HTTP（精判/OTA 下载） */
} err_module_t;

#define ERR_CODE(mod, code)  (-(((int)(mod) << 8) | (int)(code)))

/* 配置 */
#define ERR_CFG_PATH      ERR_CODE(ERR_MOD_CFG, 1)
#define ERR_CFG_PARSE     ERR_CODE(ERR_MOD_CFG, 2)
#define ERR_CFG_WRITE     ERR_CODE(ERR_MOD_CFG, 3)

/* 日志 */
#define ERR_LOG_INIT      ERR_CODE(ERR_MOD_LOG, 1)
#define ERR_LOG_WRITE     ERR_CODE(ERR_MOD_LOG, 2)

/* 传感器 */
#define ERR_SENSOR_COMM   ERR_CODE(ERR_MOD_SENSOR, 1)
#define ERR_SENSOR_STALE  ERR_CODE(ERR_MOD_SENSOR, 2)
#define ERR_SENSOR_RANGE  ERR_CODE(ERR_MOD_SENSOR, 3)
#define ERR_SENSOR_SPIKE  ERR_CODE(ERR_MOD_SENSOR, 4)

/* 摄像头 */
#define ERR_CAM_OPEN      ERR_CODE(ERR_MOD_CAM, 1)
#define ERR_CAM_FMT       ERR_CODE(ERR_MOD_CAM, 2)
#define ERR_CAM_STREAM    ERR_CODE(ERR_MOD_CAM, 3)
#define ERR_CAM_FRAME     ERR_CODE(ERR_MOD_CAM, 4)

/* 精判 */
#define ERR_DET_QUEUE     ERR_CODE(ERR_MOD_DET, 1)
#define ERR_DET_NO_FRAME  ERR_CODE(ERR_MOD_DET, 2)
#define ERR_DET_MODEL     ERR_CODE(ERR_MOD_DET, 3)

/* MQTT */
#define ERR_MQTT_CONNECT  ERR_CODE(ERR_MOD_MQTT, 1)
#define ERR_MQTT_AUTH     ERR_CODE(ERR_MOD_MQTT, 2)
#define ERR_MQTT_SEND     ERR_CODE(ERR_MOD_MQTT, 3)
#define ERR_MQTT_RECV     ERR_CODE(ERR_MOD_MQTT, 4)

/* OTA */
#define ERR_OTA_MANIFEST  ERR_CODE(ERR_MOD_OTA, 1)
#define ERR_OTA_FETCH     ERR_CODE(ERR_MOD_OTA, 2)
#define ERR_OTA_SHA       ERR_CODE(ERR_MOD_OTA, 3)
#define ERR_OTA_HMAC      ERR_CODE(ERR_MOD_OTA, 4)
#define ERR_OTA_EXTRACT   ERR_CODE(ERR_MOD_OTA, 5)
#define ERR_OTA_INSTALL   ERR_CODE(ERR_MOD_OTA, 6)

/* 时间同步 */
#define ERR_TIME_RESOLVE  ERR_CODE(ERR_MOD_TIME, 1)
#define ERR_TIME_RECV     ERR_CODE(ERR_MOD_TIME, 2)
#define ERR_TIME_SET      ERR_CODE(ERR_MOD_TIME, 3)

/* 桥接外设 */
#define ERR_BRIDGE_IIO    ERR_CODE(ERR_MOD_BRIDGE, 1)
#define ERR_BRIDGE_KEY    ERR_CODE(ERR_MOD_BRIDGE, 2)

/* 云端 HTTP */
#define ERR_CLOUD_CONNECT ERR_CODE(ERR_MOD_CLOUD, 1)
#define ERR_CLOUD_STATUS  ERR_CODE(ERR_MOD_CLOUD, 2)
#define ERR_CLOUD_AUTH    ERR_CODE(ERR_MOD_CLOUD, 3)

/** 错误码 → 可读名（"OTA:SHA"）；0 → "OK"；未知 → "UNKNOWN" */
const char *err_str(int code);

#ifdef __cplusplus
}
#endif

#endif
