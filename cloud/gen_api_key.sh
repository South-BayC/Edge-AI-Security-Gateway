#!/bin/sh
# ============================================================
# gen_api_key.sh — 生成 HTTP 接口鉴权 Key（精判 / OTA / 白名单）
#
# 生成 cloud/secrets/api.key（600），server.py 启动时读取；
# 板端通过 gateway.json 的 cloud_api_key 或 set_config 下发。
#
# 用法: ./gen_api_key.sh [自定义key]
# ============================================================
set -e

BASE=$(cd "$(dirname "$0")" && pwd)
KEY=${1:-$(openssl rand -hex 16)}
mkdir -p "$BASE/secrets"

printf '%s\n' "$KEY" > "$BASE/secrets/api.key"
chmod 600 "$BASE/secrets/api.key"

echo "API Key 已写入 $BASE/secrets/api.key"
echo "  Key: $KEY"
echo
echo "板端配置（任选其一）："
echo "  1) 远程下发（broker 可用时）："
echo "     mosquitto_pub -h 127.0.0.1 -u gateway -P '<密码>' \\"
echo "       -t 'gateway/<device_id>/cmd' \\"
echo "       -m '{\"cmd\":\"set_config\",\"id\":20,\"cloud_api_key\":\"$KEY\"}'"
echo "  2) 或编辑 /opt/edge-gateway/config/gateway.json 的 cloud_api_key 后重启应用"
