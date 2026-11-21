/**
 * net_probe.h — 网络可达性探测（infra 层）
 *
 * 供 UI 显示「联网/离线」用：对目标地址做非阻塞 TCP connect，
 * 避免 UI 直接使用 socket API。
 */
#ifndef NET_PROBE_H
#define NET_PROBE_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 探测 TCP 端口是否可连接。
 * @param ip         目标 IPv4（点分十进制）
 * @param port       目标端口
 * @param timeout_ms 连接超时（毫秒）
 * @return 1=可达 0=不可达
 */
int net_probe_tcp(const char *ip, int port, int timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* NET_PROBE_H */
