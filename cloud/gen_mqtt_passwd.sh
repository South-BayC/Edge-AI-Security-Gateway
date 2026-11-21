#!/bin/sh
# ============================================================
# gen_mqtt_passwd.sh — 生成/更新 MQTT 账号（mosquitto_passwd）
#
# 用法:
#   ./gen_mqtt_passwd.sh [user] [password]
#     不传密码则随机生成并打印
#
# 生成 cloud/secrets/mosquitto.passwd（600），由 mosquitto.conf 引用。
# ============================================================
set -e

BASE=$(cd "$(dirname "$0")" && pwd)
USER=${1:-gateway}
PASS=${2:-}
mkdir -p "$BASE/secrets"

command -v mosquitto_passwd >/dev/null 2>&1 || {
    echo "缺少 mosquitto_passwd，请安装 mosquitto-clients: sudo apt install mosquitto-clients"
    exit 1
}

if [ -z "$PASS" ]; then
    PASS=$(openssl rand -hex 8)
fi

# -c 会覆盖文件；这里统一重建（只有一个账号，简单可预期）
mosquitto_passwd -b -c "$BASE/secrets/mosquitto.passwd" "$USER" "$PASS"
chmod 600 "$BASE/secrets/mosquitto.passwd"

echo "MQTT 账号已写入 $BASE/secrets/mosquitto.passwd"
echo "  用户名: $USER"
echo "  密码  : $PASS"
echo
echo "板端一次性配置（broker 仍允许匿名时下发；或在板端编辑 gateway.json）："
echo "  mosquitto_pub -h 127.0.0.1 -t 'gateway/<device_id>/cmd' -m '{\"cmd\":\"set_config\",\"id\":9,\"mqtt_user\":\"$USER\",\"mqtt_pass\":\"$PASS\"}'"
echo
echo "启用认证后，命令行工具需要带账号："
echo "  mosquitto_sub -h 127.0.0.1 -u $USER -P $PASS -t 'gateway/#' -v"
