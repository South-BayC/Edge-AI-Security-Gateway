# PC 端单元测试（D1）
#
# 只编译纯逻辑模块（不依赖 LVGL/V4L2/ncnn），在开发机（VM）用 gcc 跑：
#   cd tests && make run
#
# 被测模块：
#   ota_hmac      SHA-256 / HMAC 标准向量 + 文件流式校验
#   config_mgr    JSON 解析/回退/裁剪/字符串字段/落盘
#   event_store   追加/去重/删除/落盘/重载/权限
#   sensor_mgr    滤波 + 故障升级 + 恢复
#   fusion        单源/双源评分与定案

CC      ?= gcc
SRC     := ../core/lvgl/src
CFLAGS  := -std=gnu99 -g -O0 -Wall -Wextra -I$(SRC)
LDLIBS  := -lm -lpthread
COMMON  := $(SRC)/log_svc.c $(SRC)/thread_util.c

TESTS   := test_hmac test_config test_event_store test_sensor_mgr test_fusion test_sensor_calib test_imu_math test_warn_policy test_warn_rule test_json_util

all: $(TESTS)

test_hmac: test_hmac.c $(SRC)/ota_hmac.c
	$(CC) $(CFLAGS) $^ -o $@ $(LDLIBS)

test_config: test_config.c $(SRC)/config_mgr.c $(COMMON)
	$(CC) $(CFLAGS) $^ -o $@ $(LDLIBS)

test_event_store: test_event_store.c $(SRC)/event_store.c $(COMMON)
	$(CC) $(CFLAGS) $^ -o $@ $(LDLIBS)

test_sensor_mgr: test_sensor_mgr.c $(SRC)/sensor_mgr.c
	$(CC) $(CFLAGS) $^ -o $@ $(LDLIBS)

test_fusion: test_fusion.c $(SRC)/fusion.c
	$(CC) $(CFLAGS) $^ -o $@ $(LDLIBS)

test_sensor_calib: test_sensor_calib.c $(SRC)/sensor_calib.c $(SRC)/config_mgr.c $(COMMON)
	$(CC) $(CFLAGS) $^ -o $@ $(LDLIBS)

test_imu_math: test_imu_math.c $(SRC)/domain/sensors/imu_math.c
	$(CC) $(CFLAGS) $^ -o $@ $(LDLIBS)

test_warn_policy: test_warn_policy.c $(SRC)/domain/alarm/warn_policy.c
	$(CC) $(CFLAGS) $^ -o $@ $(LDLIBS)

test_warn_rule: test_warn_rule.c $(SRC)/domain/warn_rule.c
	$(CC) $(CFLAGS) $^ -o $@ $(LDLIBS)

test_json_util: test_json_util.c $(SRC)/domain/json_util.c
	$(CC) $(CFLAGS) $^ -o $@ $(LDLIBS)

run: all
	@fail=0; \
	for t in $(TESTS); do \
		echo "=== $$t ==="; \
		./$$t || fail=1; \
	done; \
	if [ $$fail -eq 0 ]; then echo "ALL TESTS PASSED"; else echo "SOME TESTS FAILED"; fi; \
	exit $$fail

clean:
	rm -f $(TESTS)

.PHONY: all run clean
