# 字库生成工具

用于生成检测/设置界面使用的补充中文字库 `lv_font_ui_cn_16`（LVGL 16px/4bpp）。

## 依赖

- Node.js + `lv_font_conv`（`npm install`，已随项目放在 `node_modules/`）
- Python 3（仅用于扫描字符表）
- 字库源：`third_party/fonts/NotoSansSC-VF.ttf`（SIL OFL 1.1）

## 生成

```bash
cd tools/fontgen
python3 extract_symbols.py     # 扫描 core/lvgl/src/ui/*.c 生成 symbols.txt
./generate.sh                  # 调用 npx lv_font_conv 生成 C 文件
```

输出：`core/lvgl/src/font/lv_font_ui_cn_16.c`

## 注意

- 新增中文标签后重新执行一次，否则字库缺字会显示方框/乱码；
- `symbols.txt` 只包含 UI 代码里出现的汉字与全角标点；
- **扫描是递归的**（`core/lvgl/src/ui/` 下的 `panels/`、`modals/` 也会扫描），
  早先版本只扫了 `ui/*.c`，拆分后导致 86 个汉字缺字，已修复；
- 生成文件较大（约 600KB 源码），属正常。
