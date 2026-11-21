#!/bin/sh
# ============================================================
# build_mbedtls.sh — 交叉编译 mbedTLS 2.28 LTS（T11 单向 TLS 依赖）
#
# 产物：third_party/mbedtls-armhf/{include,lib/libmbedtls.a,...}
# core/lvgl/Makefile 检测到 libmbedtls.a 后会自动加 -DMQTT_TLS。
#
# 用法（开发机 VM）：
#   ./scripts/build_mbedtls.sh
#   MBEDTLS_VER=2.28.8 ./scripts/build_mbedtls.sh
# ============================================================
set -e

BASE=$(cd "$(dirname "$0")/.." && pwd)
VER="${MBEDTLS_VER:-2.28.8}"
SRC="$BASE/third_party/mbedtls"
OUT="$BASE/third_party/mbedtls-armhf"
TARBALL="$BASE/third_party/mbedtls-$VER.tar.gz"
URL="https://github.com/Mbed-TLS/mbedtls/archive/refs/tags/v$VER.tar.gz"

CC_BIN="${CROSS_COMPILE:-arm-linux-gnueabihf-}gcc"
# mbedTLS 2.28 源码使用 C99 行内声明；GCC 4.9 默认 gnu89，需显式加 -std=gnu99。
# 放进 CC 而不是 CFLAGS：命令行 CFLAGS 会覆盖 mbedTLS Makefile 里的 include 路径。
CC_CMD="$CC_BIN -std=gnu99"
AR_BIN="${CROSS_COMPILE:-arm-linux-gnueabihf-}ar"

if [ ! -x "$(command -v "$CC_BIN" 2>/dev/null)" ]; then
    echo "错误：找不到 $CC_BIN（检查 CROSS_COMPILE）"
    exit 1
fi

if [ -d "$SRC" ] && [ -f "$SRC/include/mbedtls/ssl.h" ]; then
    echo "源码已存在：$SRC"
else
    if [ ! -f "$TARBALL" ]; then
        echo "下载 mbedTLS $VER ..."
        curl -fL --retry 3 -o "$TARBALL" "$URL" || wget -O "$TARBALL" "$URL"
    fi
    echo "解压到 third_party/mbedtls ..."
    mkdir -p "$SRC"
    tar -xzf "$TARBALL" -C "$SRC" --strip-components=1
fi

echo "交叉编译（$CC_CMD）..."
make -C "$SRC" clean >/dev/null 2>&1 || true
make -C "$SRC" lib CC="$CC_CMD" AR="$AR_BIN" \
    -j"$(nproc 2>/dev/null || echo 4)"

mkdir -p "$OUT/lib" "$OUT/include"
cp "$SRC/library/libmbedtls.a" \
   "$SRC/library/libmbedx509.a" \
   "$SRC/library/libmbedcrypto.a" "$OUT/lib/"
cp -r "$SRC/include/mbedtls" "$OUT/include/"

echo "完成：$OUT"
echo "  lib: $(ls "$OUT/lib")"
echo "重新构建网关：cd $BASE && make lvgl"
