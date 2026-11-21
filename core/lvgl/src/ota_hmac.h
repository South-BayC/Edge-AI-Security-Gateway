/**
 * ota_hmac.h — OTA 包完整性认证（SHA-256 / HMAC-SHA256，无外部依赖）
 *
 * 用途：板端没有 openssl 命令时，用它校验 OTA 包未被篡改。
 * 说明：HMAC 是对称认证（服务端与设备共享密钥），能防篡改，
 *       但设备被攻破后密钥会泄露；生产环境建议改用非对称签名
 *       （mbedTLS/openssl 的 RSA/ECDSA 验签）。
 */
#ifndef OTA_HMAC_H
#define OTA_HMAC_H

#include <stddef.h>
#include <stdint.h>

/** 计算内存数据的 SHA-256（hex 输出 64 字符 + '\0'） */
int sha256_hex(const void *data, size_t len, char *out_hex, int out_n);

/**
 * 计算文件 HMAC-SHA256（hex 输出 64 字符 + '\0'）
 * @param path    文件路径
 * @param key     文本密钥（以 strlen 长度参与 HMAC）
 * @param out_hex 输出缓冲（≥65 字节）
 * @return 0 成功；-1 打开/读取失败
 */
int hmac_sha256_file_hex(const char *path, const char *key,
                         char *out_hex, int out_n);

#endif
