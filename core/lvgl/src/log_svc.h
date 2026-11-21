/**
 * log_svc.h — 分级异步日志服务
 *
 * 设计目标（v2 T2）：
 *   - 分级：DEBUG / INFO / WARN / ERROR / FATAL；
 *   - 异步：业务线程只写内存队列，独立线程落盘，避免采集/UI 线程被磁盘 IO 阻塞；
 *   - 轮转：单文件默认 256KB，保留 4 个历史文件；
 *   - 控制台：同时输出到 stderr，便于串口调试；
 *   - 无第三方依赖，日志点通过 LOG* 宏调用。
 *
 * 默认目录（log_init(NULL) 时按顺序探测）：
 *   1) 环境变量 GATEWAY_LOG_DIR
 *   2) /opt/edge-gateway/logs
 *   3) /var/log/edge-gateway
 *   4) ./logs
 *
 * 日志级别可用环境变量 GATEWAY_LOG_LEVEL 覆盖：
 *   debug / info / warn / error / fatal
 *
 * 用法：
 *   log_init(NULL);
 *   LOGI("BRIDGE", "device %s opened", path);
 *   LOGE("CAM", "S_FMT failed: %s", strerror(errno));
 *   log_close();
 */
#ifndef LOG_SVC_H
#define LOG_SVC_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LOG_DEBUG = 0,
    LOG_INFO,
    LOG_WARN,
    LOG_ERROR,
    LOG_FATAL
} log_level_t;

/**
 * 初始化日志服务（创建目录、打开文件、启动异步线程）。
 * @param dir 指定目录；NULL 时按候选路径探测
 * @return 0=成功；-1=无可用目录/文件（此时日志回退到 stderr）
 */
int log_init(const char *dir);

/** 停止异步线程并关闭文件（可重复调用） */
void log_close(void);

/** 设置/获取当前日志级别 */
void log_set_level(log_level_t lv);
log_level_t log_get_level(void);

/**
 * 写一条日志（线程安全，异步落盘）。
 * @param lv     级别
 * @param module 模块名（如 "BRIDGE"/"CAM"/"DET"）
 * @param fmt    printf 格式
 */
void log_write(log_level_t lv, const char *module, const char *fmt, ...);

/** 当前日志目录（未初始化时为空串） */
const char *log_dir(void);

/** 当前日志文件完整路径；成功返回 0 */
int log_file_path(char *out, int n);

/* 便捷宏 */
#define LOGD(mod, ...) log_write(LOG_DEBUG, mod, __VA_ARGS__)
#define LOGI(mod, ...) log_write(LOG_INFO,  mod, __VA_ARGS__)
#define LOGW(mod, ...) log_write(LOG_WARN,  mod, __VA_ARGS__)
#define LOGE(mod, ...) log_write(LOG_ERROR, mod, __VA_ARGS__)
#define LOGF(mod, ...) log_write(LOG_FATAL, mod, __VA_ARGS__)

#ifdef __cplusplus
}
#endif

#endif /* LOG_SVC_H */
