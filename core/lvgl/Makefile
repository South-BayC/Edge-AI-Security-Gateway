# SPDX-License-Identifier: GPL-2.0
# LVGL v9.2.2 安防网关 — 交叉编译主 Makefile
# 产物: $(BUILD_DIR)/lvgl/lvgl_gateway

CROSS_COMPILE ?= arm-linux-gnueabihf-
CC  := $(CROSS_COMPILE)gcc
LVGL_DIR := $(CURDIR)/lvgl
BUILD_LVGL_DIR := $(BUILD_DIR)/lvgl
BUILD_CACHE_LVGL := $(BUILD_DIR)/cache/lvgl
# 结果（可执行文件）放 build/lvgl/；编译过程文件（*.o）统一放 build/cache/lvgl/（源码目录保持干净）
TARGET := $(BUILD_LVGL_DIR)/lvgl_gateway

# 板端：排除桌面驱动（SDL/Wayland/X11/Windows/GLFW），避免 SDL2 依赖
LVGL_SRCS_ALL := $(shell find $(LVGL_DIR)/src -name "*.c" 2>/dev/null | sed "s|^$(CURDIR)/||")
LVGL_SRCS := $(shell find $(LVGL_DIR)/src -name "*.c" -not -path "*/drivers/sdl/*" -not -path "*/drivers/wayland/*" -not -path "*/drivers/x11/*" -not -path "*/drivers/windows/*" -not -path "*/drivers/glfw/*" 2>/dev/null | sed "s|^$(CURDIR)/||")
APP_SRCS  := $(shell find src -name "*.c" -not -name "sim_main.c" 2>/dev/null | sed "s|^$(CURDIR)/||")
APP_SRCS  := $(filter-out %test_%.c,$(APP_SRCS))
SRCS      := $(LVGL_SRCS) $(APP_SRCS)

OBJS := $(patsubst %.c,$(BUILD_CACHE_LVGL)/%.o,$(SRCS))

# ncnn（交叉编译产物：仓库根 third_party/ncnn-armhf，SCRFD 人脸检测运行时）
NCNN_DIR := ../../third_party/ncnn-armhf

# 精判模块（C++ 编译单元：NCNN SCRFD 人脸检测；NEON 加速颜色转换）
DET_CPP := src/detector.cpp
OBJS    += $(BUILD_CACHE_LVGL)/$(DET_CPP:.cpp=.o)

# T11-③：mbedTLS（可选）。检测到静态库时自动启用 TLS，否则仍可编 1883 明文版。
MBEDTLS_DIR ?= ../../third_party/mbedtls-armhf
ifneq ($(wildcard $(MBEDTLS_DIR)/lib/libmbedtls.a),)
TLS_CFLAGS := -DMQTT_TLS -I$(MBEDTLS_DIR)/include
TLS_LIBS   := -L$(MBEDTLS_DIR)/lib -lmbedtls -lmbedx509 -lmbedcrypto
else
TLS_CFLAGS :=
TLS_LIBS   :=
endif

CFLAGS  := -std=gnu99 -I$(CURDIR) -I$(CURDIR)/src -I$(LVGL_DIR) -I$(LVGL_DIR)/src -O2 -Wall -Wextra -DLV_CONF_INCLUDE_SIMPLE -DLV_LVGL_H_INCLUDE_SIMPLE -MMD -MP $(TLS_CFLAGS)
LDFLAGS := -lm -lpthread -lstdc++ -L$(CURDIR)/$(NCNN_DIR)/lib -lncnn $(TLS_LIBS)

# LVGL 自带源码的 -Wtype-limits 噪声（calendar 等第三方代码），仅对 lvgl/ 下目标屏蔽
$(BUILD_CACHE_LVGL)/lvgl/%.o: CFLAGS += -Wno-type-limits

# 自动头文件依赖：任何 .h 变化都会触发依赖它的 .c/.cpp 重编
# （此前只跟踪 lv_conf.h，改结构体头文件后旧 .o 会造成 ABI 不一致）
# 注意：-include 必须在 .DEFAULT_GOAL 之后（否则 .d 里的目标会被当成默认目标）
.DEFAULT_GOAL := all
DEPS := $(OBJS:.o=.d)
-include $(DEPS)

all: $(TARGET)
	@echo "LVGL gateway built: $(TARGET)  ($$(du -h $(TARGET) 2>/dev/null | cut -f1))"

$(TARGET): $(OBJS)
	@mkdir -p $(dir $@)
	$(CC) $^ -o $@ $(LDFLAGS)
	@arm-linux-gnueabihf-strip $@ 2>/dev/null || true

# lv_conf.h 变化时自动触发全量重编（作为所有 .o 的依赖）
$(BUILD_CACHE_LVGL)/%.o: %.c $(CURDIR)/lv_conf.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# cam_feed 转换循环为帧率热点：单独 -O3 + NEON（行转换 8 像素/迭代）
$(BUILD_CACHE_LVGL)/src/cam_feed.o: src/cam_feed.c $(CURDIR)/lv_conf.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -O3 -mfpu=neon -c $< -o $@

# 精判模块（C++：NCNN SCRFD 人脸检测；NEON 加速颜色转换）
# 编译器显式指定交叉 g++（不依赖 CXX 变量解析）
$(BUILD_CACHE_LVGL)/src/detector.o: $(DET_CPP) $(CURDIR)/lv_conf.h $(NCNN_DIR)/include/ncnn/net.h
	@mkdir -p $(dir $@)
	arm-linux-gnueabihf-g++ -std=c++11 -I$(CURDIR) -I$(LVGL_DIR) -I$(LVGL_DIR)/src \
	       -I$(CURDIR)/$(NCNN_DIR)/include \
	       -O2 -Wall -mfpu=neon -DLV_CONF_INCLUDE_SIMPLE -DLV_LVGL_H_INCLUDE_SIMPLE -MMD -MP -c $< -o $@

# 模拟器（SDL，宿主机）
SIM_TARGET := $(BUILD_LVGL_DIR)/lvgl_sim
SIM_SRCS   := $(filter-out src/main.c,$(APP_SRCS)) src/sim_main.c $(LVGL_SRCS_ALL)
SIM_OBJS   := $(patsubst %.c,$(BUILD_CACHE_LVGL)/sim/%.o,$(SIM_SRCS))
SIM_CFLAGS := -std=gnu99 -g -O0 -I$(CURDIR) -I$(LVGL_DIR) -I$(LVGL_DIR)/src -Wall -Wextra -DLV_CONF_INCLUDE_SIMPLE -DLV_LVGL_H_INCLUDE_SIMPLE -DLV_USE_SDL=1

sim: $(SIM_TARGET)
	@echo "LVGL sim built: $(SIM_TARGET)"

$(SIM_TARGET): $(SIM_OBJS)
	@mkdir -p $(dir $@)
	gcc $^ -o $@ -lSDL2 -lm -lpthread

$(BUILD_CACHE_LVGL)/sim/%.o: %.c $(CURDIR)/lv_conf.h
	@mkdir -p $(dir $@)
	gcc $(SIM_CFLAGS) -c $< -o $@

clean:
	rm -rf $(BUILD_LVGL_DIR) $(BUILD_CACHE_LVGL)

# LVGL 无独立板端测试程序（交互验证用模拟器: make sim）
test:
	@echo "LVGL: no standalone test, run 'make sim' on host for UI verification"

.PHONY: all clean sim test
