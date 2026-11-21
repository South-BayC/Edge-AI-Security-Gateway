/**
 * ota_mgr.h — 应用级 OTA（T9）
 *
 * 目录（默认 /opt/edge-gateway，可用 GATEWAY_OTA_DIR 覆盖）：
 *   releases/<version>/lvgl_gateway
 *   current -> releases/<version>
 *   ota/packages/            下载/放置升级包
 *   ota/staging/             解压暂存
 *   ota/state.json           升级状态（掉电恢复）
 *
 * 升级包目录（pkg_dir）：
 *   manifest.json            product/hardware/version
 *   payload.tar.gz           解压后根目录直接是 lvgl_gateway（或资源文件）
 *   checksums.sha256         sha256sum -c 可校验
 *   signature.sig            预留（当前只检查存在性，不验签）
 *
 * 状态机：
 *   IDLE → INSTALLING → PENDING_CONFIRM → CONFIRMED
 *                       ↘ (下次启动未确认/安装中断) → ROLLED_BACK
 *
 * 依赖板端命令：sha256sum、tar、cp、rm（BusyBox 通常自带）。
 */
#ifndef OTA_MGR_H
#define OTA_MGR_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    OTA_IDLE = 0,
    OTA_INSTALLING,
    OTA_PENDING_CONFIRM,
    OTA_CONFIRMED,
    OTA_ROLLED_BACK,
    OTA_FAILED
} ota_state_t;

/** 初始化目录/状态；安装中断或未确认启动超限时自动回滚 */
int ota_init(void);

/**
 * 从本地包目录安装并原子切换 current。
 * @param pkg_dir 升级包目录（含 manifest.json/payload.tar.gz/checksums.sha256）
 * @return 0=成功；-1=失败（状态置 FAILED）
 */
int ota_install(const char *pkg_dir);

/**
 * 网络 OTA（拉取式）：GET manifest_url → 版本比对 → 下载包 → sha256 校验
 * → 解包 → ota_install()。
 * @param manifest_url 形如 http://host:port/ota/manifest.json
 * @return 1=已安装（需重启生效）；0=无更新/跳过；-1=失败
 */
int ota_check(const char *manifest_url);

/**
 * 启动 OTA 后台线程（定时拉取 + 供 MQTT 下发触发）
 * @param url            manifest 地址（空串表示仅接受 MQTT 触发）
 * @param interval_min   定时检查间隔（分钟，0=只接受 MQTT 触发）
 * @return 0=成功；-1=线程创建失败
 */
int ota_periodic_start(const char *url, int interval_min);

/** 停止 OTA 后台线程并 join（退出流程调用） */
void ota_periodic_stop(void);

/**
 * MQTT 下发触发一次检查（非阻塞；线程未启动时同步执行）
 * @param url manifest 地址
 * @return 0=已受理；-1=参数非法
 */
int ota_request_check(const char *url);

/** 主循环轮询：1=新版本已安装，需要重启生效（读后清零） */
int ota_take_installed(void);

/** 健康检查通过后确认新版本（PENDING_CONFIRM → CONFIRMED） */
int ota_confirm(void);

/** 手动回滚到 previous */
int ota_rollback(void);

ota_state_t ota_get_state(void);
const char *ota_state_name(ota_state_t st);

/** 当前版本（读 current 符号链接的 basename；无则空串） */
const char *ota_current_version(void);

/** 是否处于待确认状态 */
int ota_pending_confirm(void);

/** OTA 根目录（只读） */
const char *ota_root(void);

#ifdef __cplusplus
}
#endif

#endif /* OTA_MGR_H */
