/**
 * time_sync.h — 最小 SNTP 时间同步（UDP 123）
 *
 * 目的：板端无 RTC 电池/DNS 时，日志时间停在 1970，证书有效期校验也会失败。
 * 方案：后台线程向 NTP/SNTP 服务器取时并 settimeofday：
 *   - 启动后立即尝试一次，失败每 5 分钟重试；
 *   - 成功后每 6 小时再校准一次（抵消 RTC/晶振漂移）；
 *   - 服务器：GATEWAY_NTP_SERVER 环境变量，默认 192.168.3.26（开发机）。
 */
#ifndef TIME_SYNC_H
#define TIME_SYNC_H

/**
 * 启动同步线程（幂等）
 * @param server 服务器地址（IP 或域名）；NULL/空则用 GATEWAY_NTP_SERVER 或默认值
 */
void time_sync_start(const char *server);

/** 停止同步线程并 join（退出流程调用） */
void time_sync_stop(void);

#endif
