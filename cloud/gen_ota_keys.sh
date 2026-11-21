#!/bin/sh
# ============================================================
# gen_ota_keys.sh — 生成 OTA 包认证密钥（HMAC-SHA256，只需执行一次）
#
#   cloud/secrets/ota.key   32 字节随机密钥（hex 文本）
#                        服务端用于签名，板端用于校验
#
# 说明：板端没有 openssl，因此这里用对称 HMAC 而非 RSA 签名。
#       能防包篡改；但设备被攻破会泄露密钥，生产建议改用
#       mbedTLS/openssl 的非对称验签。
#
# 用法: ./gen_ota_keys.sh
# ============================================================
set -e

BASE=$(cd "$(dirname "$0")" && pwd)
KEYS="$BASE/secrets"

mkdir -p "$KEYS"

if [ -f "$KEYS/ota.key" ]; then
    echo "已存在: $KEYS/ota.key（如需更换请先删除 secrets/ota.key）"
    exit 0
fi

openssl rand -hex 32 > "$KEYS/ota.key"
chmod 600 "$KEYS/ota.key"

echo "认证密钥已生成: $KEYS/ota.key（保密；部署到板端 /opt/edge-gateway/ota/ota.key）"
