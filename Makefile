# SPDX-License-Identifier: GPL-2.0
# Top-level Makefile for imx6ull-drivers project
# Orchestrates core (kernel modules), dts (device tree) and lvgl (applications)
# All build outputs go to build/ — source directories stay clean.

CROSS_COMPILE ?= arm-linux-gnueabihf-
BUILD_DIR := $(CURDIR)/build

# 默认构建项目所需的全部文件（内核除外）：
#   core  -> 8 个内核驱动模块 -> build/module/*.ko
#   dts   -> 设备树            -> build/dts/*.dtb
#   lvgl  -> LVGL 应用         -> build/lvgl/lvgl_gateway
all: core dts lvgl

core:
	$(MAKE) -C core BUILD_DIR=$(BUILD_DIR)

dts:
	$(MAKE) -C dts BUILD_DIR=$(BUILD_DIR)

lvgl:
	$(MAKE) -C core/lvgl BUILD_DIR=$(BUILD_DIR)

clean:
	$(MAKE) -C core clean
	$(MAKE) -C dts clean
	$(MAKE) -C core/lvgl clean
	rm -rf $(BUILD_DIR)

# 宿主机单元测试（D1，不需要板子）
test:
	$(MAKE) -C tests run

clean-tests:
	$(MAKE) -C tests clean

# 本地路径覆盖（不入 git）：NFS_ROOT / TFTP_DIR / 工具链等
-include config.mk

# 部署到板端（内核模块 + 设备树 + 应用 + 模型 + OTA 当前版本）
# 用法：板端先 killall lvgl_gateway，再在 VM 执行 make deploy
# 注意：/opt/edge-gateway 权限为 0700（安全考虑），解析/复制需 sudo
NFS_ROOT   ?= /home/szh/linux/nfs/rootfs
TFTP_DIR   ?= /home/szh/linux/tftp
MODULE_DIR ?= $(NFS_ROOT)/lib/modules/4.1.15

deploy: core dts lvgl
	@echo "提醒：请先在板端执行 killall lvgl_gateway，再启动新版"
	sudo cp ./build/module/*.ko $(MODULE_DIR)/ -f
	sudo cp ./build/dts/*.dtb $(TFTP_DIR)/ -f
	sudo cp ./build/lvgl/lvgl_gateway $(NFS_ROOT)/usr/bin/ -f
	sudo cp ./models/face.param ./models/face.bin $(NFS_ROOT)/root/ -f
	@rel=$$(sudo readlink -f $(NFS_ROOT)/opt/edge-gateway/current 2>/dev/null); \
	if sudo test -d "$$rel"; then \
		sudo cp ./build/lvgl/lvgl_gateway "$$rel/" -f; \
		echo "deployed -> $$rel"; \
	else \
		echo "skip release copy: current 未指向有效目录 ($$rel)"; \
	fi
	@if [ -f ./cloud/secrets/ota.key ]; then \
		sudo mkdir -p $(NFS_ROOT)/opt/edge-gateway/ota; \
		sudo cp ./cloud/secrets/ota.key $(NFS_ROOT)/opt/edge-gateway/ota/ota.key -f; \
		sudo chmod 600 $(NFS_ROOT)/opt/edge-gateway/ota/ota.key; \
		echo "deployed -> opt/edge-gateway/ota/ota.key"; \
	else \
		echo "skip ota key (cloud/secrets/ota.key not found)"; \
	fi
	@if [ -f ./cloud/certs/ca.crt ]; then \
		sudo mkdir -p $(NFS_ROOT)/opt/edge-gateway/certs; \
		sudo cp ./cloud/certs/ca.crt $(NFS_ROOT)/opt/edge-gateway/certs/ -f; \
		sudo chmod 644 $(NFS_ROOT)/opt/edge-gateway/certs/ca.crt; \
		echo "deployed -> opt/edge-gateway/certs/ca.crt"; \
	else \
		echo "skip CA (run cloud/gen_tls_certs.sh first)"; \
	fi

.PHONY: all core dts lvgl clean deploy test clean-tests
