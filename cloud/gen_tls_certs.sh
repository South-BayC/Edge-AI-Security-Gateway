#!/bin/sh
# ============================================================
# gen_tls_certs.sh — 生成 MQTT TLS 用的自签 CA + 服务器证书（T11）
#
# 输出（cloud/certs/）：
#   ca.crt / ca.key          自签 CA（ca.crt 需部署到板端）
#   server.crt / server.key  mosquitto 服务端证书（SAN 含 broker IP）
#
# 用法：
#   ./gen_tls_certs.sh                 # 已存在则跳过
#   ./gen_tls_certs.sh --force         # 强制重新生成
#   BROKER_IP=192.168.3.26 ./gen_tls_certs.sh
# ============================================================
set -e

BASE=$(cd "$(dirname "$0")" && pwd)
OUT="$BASE/certs"
BROKER_IP="${BROKER_IP:-192.168.3.26}"

mkdir -p "$OUT"

if [ -f "$OUT/ca.crt" ] && [ -f "$OUT/server.crt" ] && [ "$1" != "--force" ]; then
    echo "certs 已存在：$OUT（如需重生成用 --force）"
    exit 0
fi

echo "生成 CA 与服务器证书（broker IP=$BROKER_IP）..."

openssl genrsa -out "$OUT/ca.key" 2048 >/dev/null 2>&1
openssl req -x509 -new -nodes -key "$OUT/ca.key" -sha256 -days 3650 \
    -subj "/CN=Edge-GW Dev CA" -out "$OUT/ca.crt"

openssl genrsa -out "$OUT/server.key" 2048 >/dev/null 2>&1
openssl req -new -key "$OUT/server.key" -subj "/CN=$BROKER_IP" \
    -out "$OUT/server.csr"

cat > "$OUT/server.ext" <<EOF
# mbedTLS 2.28 只校验 dNSName（不识别 iPAddress SAN），
# 因此除了标准 IP SAN，额外放一条同名 DNS，保证板端 IP 直连也能通过。
subjectAltName = IP:$BROKER_IP, IP:127.0.0.1, DNS:localhost, DNS:$BROKER_IP
extendedKeyUsage = serverAuth
EOF

openssl x509 -req -in "$OUT/server.csr" -CA "$OUT/ca.crt" -CAkey "$OUT/ca.key" \
    -CAcreateserial -out "$OUT/server.crt" -days 825 -sha256 \
    -extfile "$OUT/server.ext" >/dev/null 2>&1

rm -f "$OUT/server.csr" "$OUT/server.ext" "$OUT/ca.srl"
chmod 600 "$OUT/ca.key" "$OUT/server.key"
chmod 644 "$OUT/ca.crt" "$OUT/server.crt"

echo "完成："
echo "  CA     : $OUT/ca.crt   （部署到板端 /opt/edge-gateway/certs/）"
echo "  服务端 : $OUT/server.crt / server.key（mosquitto 使用）"
echo "校验：openssl verify -CAfile $OUT/ca.crt $OUT/server.crt"
