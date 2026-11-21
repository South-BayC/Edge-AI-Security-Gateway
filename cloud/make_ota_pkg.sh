#!/bin/sh
# ============================================================
# make_ota_pkg.sh — 生成 OTA 包到 cloud/ota/（由 server.py 的 /ota/ 路由提供下载）
#
# 用法:
#   ./make_ota_pkg.sh <version> [binary]
#     version : 版本号，例如 1.2.0
#     binary  : 默认 ../build/lvgl/lvgl_gateway
#
# 生成:
#   cloud/ota/pkg-<version>.tar.gz   （内层: manifest.json + payload.tar.gz + checksums.sha256）
#   cloud/ota/manifest.json          （板端 /ota/manifest.json 读取）
# ============================================================
set -e

BASE=$(cd "$(dirname "$0")" && pwd)
VER=${1:?用法: $0 <version> [binary]}
BIN=${2:-$BASE/../build/lvgl/lvgl_gateway}
OUT="$BASE/ota"
ROOT="$OUT/pkgroot"
KEY="$BASE/secrets/ota.key"

[ -f "$BIN" ] || { echo "binary not found: $BIN"; exit 1; }
[ -f "$KEY" ] || { echo "missing key: $KEY（先执行 ./gen_ota_keys.sh）"; exit 1; }

rm -rf "$ROOT"
mkdir -p "$ROOT/payload" "$OUT"
cp "$BIN" "$ROOT/payload/lvgl_gateway"
chmod +x "$ROOT/payload/lvgl_gateway"

printf '{"product":"edge-ai-gateway","hardware":"imx6ull","version":"%s"}\n' \
    "$VER" > "$ROOT/manifest.json"

tar -czf "$ROOT/payload.tar.gz" -C "$ROOT/payload" lvgl_gateway
( cd "$ROOT" && sha256sum payload.tar.gz > checksums.sha256 )

tar -czf "$OUT/pkg-$VER.tar.gz" -C "$ROOT" \
    manifest.json payload.tar.gz checksums.sha256

SHA=$(sha256sum "$OUT/pkg-$VER.tar.gz" | cut -d' ' -f1)

# T11：HMAC-SHA256 认证（板端内置实现校验，失败拒绝安装）
HMAC=$(openssl dgst -sha256 -hmac "$(cat "$KEY")" -r "$OUT/pkg-$VER.tar.gz" | cut -d' ' -f1)

printf '{"product":"edge-ai-gateway","hardware":"imx6ull","version":"%s","package":"pkg-%s.tar.gz","sha256":"%s","hmac":"%s"}\n' \
    "$VER" "$VER" "$SHA" "$HMAC" > "$OUT/manifest.json"

echo "OK: $OUT/pkg-$VER.tar.gz"
cat "$OUT/manifest.json"
echo "板端: GATEWAY_OTA_URL=http://<VM-IP>:8000/ota/manifest.json"
