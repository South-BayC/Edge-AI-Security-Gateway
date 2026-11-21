#!/usr/bin/env python3
"""
extract_symbols.py — 收集 UI 代码中出现的汉字/全角标点，生成字库字符表。

扫描范围：core/lvgl/src/ui/*.c（可按需要扩展）。
输出：tools/fontgen/symbols.txt
"""
import os
import sys

BASE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(BASE, "..", ".."))
UI_DIR = os.path.join(ROOT, "core", "lvgl", "src", "ui")


def is_cjk(ch: str) -> bool:
    o = ord(ch)
    return (0x4E00 <= o <= 0x9FFF or
            0x3000 <= o <= 0x303F or
            0xFF00 <= o <= 0xFFEF or
            # 常用中文排版标点（不在 CJK 区块，需显式纳入）
            o in (0x2026, 0x00B7, 0x2014, 0x2018, 0x2019, 0x201C, 0x201D,
                  0x2022, 0x00D7, 0x2192, 0x2190, 0x2191, 0x2193))


def iter_sources() -> int:
    """递归扫描 ui/ 下所有 .c/.h（panels/、modals/ 也需覆盖）。"""
    count = 0
    for base, _dirs, files in os.walk(UI_DIR):
        for name in sorted(files):
            if name.endswith((".c", ".h")):
                count += 1
                yield os.path.join(base, name)
    return count


def main() -> int:
    chars = set()
    for path in iter_sources():
        with open(path, encoding="utf-8") as f:
            for ch in f.read():
                if is_cjk(ch):
                    chars.add(ch)

    out = "".join(sorted(chars))
    with open(os.path.join(BASE, "symbols.txt"), "w", encoding="utf-8") as f:
        f.write(out)

    print("chars:", len(chars))
    return 0


if __name__ == "__main__":
    sys.exit(main())
