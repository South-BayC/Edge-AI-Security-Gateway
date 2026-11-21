/**
 * http_get.h — 最小 HTTP/1.1 GET 下载（无第三方依赖）
 *
 * 设计对齐 cloud_detect.c：
 *   - 裸 TCP + HTTP/1.1，只支持 http://（https 留到 T11 安全阶段）
 *   - 非阻塞 connect + select 超时，死服务快速失败
 *   - Connection: close，读至对端关闭
 *   - 只关心 200 响应，其余一律失败
 *
 * 用途：OTA 拉取 manifest.json 与安装包（T9 网络侧）。
 */
#ifndef HTTP_GET_H
#define HTTP_GET_H

/**
 * http_get_file - 下载 URL 到本地文件
 * @url:      http://host[:port]/path
 * @out_path: 输出文件路径（已存在则覆盖）
 * 返回：0 成功；-1 失败（原因见日志）
 */
int http_get_file(const char *url, const char *out_path);

/**
 * 设置 HTTP 请求头 X-API-Key（云端鉴权，T11-B2）
 * @param key 空串/NULL 表示不携带
 */
void http_get_set_api_key(const char *key);

#endif
