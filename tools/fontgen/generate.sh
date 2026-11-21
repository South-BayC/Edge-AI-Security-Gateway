#!/bin/sh
# 生成 LVGL 中文字库（Noto Sans SC，OFL）
# 用法：./generate.sh
set -e
BASE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$BASE/../.." && pwd)

FONT="$ROOT/third_party/fonts/NotoSansSC-VF.ttf"
[ -f "$FONT" ] || { echo "缺少字库源: $FONT"; exit 1; }

python3 "$BASE/extract_symbols.py"
SYMS=$(cat "$BASE/symbols.txt")

npx lv_font_conv \
    --font "$FONT" \
    --size 16 --bpp 4 \
    --format lvgl --lv-include lvgl.h \
    --no-compress --force-fast-kern-format \
    -r 0x20-0x7F \
    --symbols "$SYMS" \
    -o "$ROOT/core/lvgl/src/font/lv_font_ui_cn_16.c"

echo "生成完成: core/lvgl/src/font/lv_font_ui_cn_16.c"
