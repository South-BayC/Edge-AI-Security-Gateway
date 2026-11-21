# 第三方依赖清单

> 所有 C/C++ 依赖都放在项目内 `third_party/`（该目录不入 git，便于控制体积）。
> 重新搭建环境时按本清单准备；版本/来源变更必须同步更新本文件。

## 1. C/C++ 库

| 依赖 | 位置 | 用途 | 说明 |
|---|---|---|---|
| ncnn（源码） | `third_party/ncnn` | 人脸检测运行时源码 | 仅作源码参考，板端链接的是 `ncnn-armhf` |
| ncnn（armhf） | `third_party/ncnn-armhf` | 交叉编译静态/动态库 | `lib/libncnn.a`、`include/ncnn` |
| mbedTLS 2.28.8（源码） | `third_party/mbedtls` | MQTT TLS | 由 `scripts/build_mbedtls.sh` 下载/解压 |
| mbedTLS（armhf） | `third_party/mbedtls-armhf` | 交叉编译静态库 | `libmbedtls/libmbedx509/libmbedcrypto` |
| 工具链配置 | `third_party/build-armhf/toolchain.cmake` | ncnn 交叉编译 | 参考用 |

### 构建时如何被引用

- `core/lvgl/Makefile`：
  - `NCNN_DIR := ../../third_party/ncnn-armhf`
  - `MBEDTLS_DIR ?= ../../third_party/mbedtls-armhf`（检测到 `libmbedtls.a` 自动启用 TLS）

### 已清理

| 目录 | 原因 |
|---|---|
| `third_party/ncnn-assets`（1.69GB） | 仅模型转换素材，构建不引用；实际模型在 `models/` |
| `third_party/libfacedetection`（34MB） | 构建不引用 |

## 2. 模型

| 文件 | 位置 | 来源 |
|---|---|---|
| SCRFD-500M-opt2 `face.param` / `face.bin` | `models/` | ncnn-assets 转换产物 |
| YOLOv8n `yolov8n.pt` | `cloud/` | 云端复判（ultralytics） |

## 3. 字库

| 文件 | 位置 | 许可 |
|---|---|---|
| Noto Sans SC 可变字体 | `third_party/fonts/NotoSansSC-VF.ttf` | SIL OFL 1.1（同目录 `OFL.txt`） |
| 生成的中文字库 | `core/lvgl/src/font/lv_font_ui_cn_16.c` | 由上面字体生成，随项目分发 |

重新生成：

```bash
cd tools/fontgen
python3 extract_symbols.py            # 扫描 ui_home.c 生成 symbols.txt
./generate.sh                         # 生成 core/lvgl/src/font/lv_font_ui_cn_16.c
```

## 4. Python（云端）

| 包 | 用途 |
|---|---|
| fastapi / uvicorn | HTTP 服务（精判/OTA/命令） |
| ultralytics | 云端 YOLOv8 复判 |
| numpy | 图像数组 |
| face_recognition | 白名单比对 |
| paho-mqtt | 命令 ACK/重试 |

版本固定见 `cloud/requirements.txt`；离线安装可执行：

```bash
pip download -r cloud/requirements.txt -d third_party/wheels
pip install --no-index --find-links third_party/wheels -r cloud/requirements.txt
```

## 5. 外部工具（不随项目分发）

| 工具 | 用途 |
|---|---|
| `arm-linux-gnueabihf-` 工具链 | 交叉编译 |
| `lv_font_conv`（npm，`tools/fontgen/node_modules`） | 生成 LVGL 字库 |
| `mosquitto` / `mosquitto-clients` | MQTT broker 与调试 |
| `openssl` | 证书/密钥生成（VM） |
