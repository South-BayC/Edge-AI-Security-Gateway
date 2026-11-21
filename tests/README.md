# 单元测试（D1）

纯逻辑模块的宿主机测试，不依赖 LVGL/V4L2/ncnn，在开发机（VM）上运行。

## 运行

```bash
cd tests
make run          # 编译并依次运行全部用例
make clean        # 清理产物
```

预期最后一行：

```
ALL TESTS PASSED
```

## 覆盖范围

| 用例 | 被测模块 | 内容 |
|---|---|---|
| `test_hmac` | `ota_hmac.c` | SHA-256/HMAC 标准向量、文件流式校验 |
| `test_config` | `config_mgr.c` | 默认值、JSON 解析/落盘/重载、字符串字段、越界裁剪、损坏回退 |
| `test_event_store` | `event_store.c` | 追加/按 id 去重/删除/落盘/重载/权限 600 |
| `test_sensor_mgr` | `sensor_mgr.c` | 滤波输出、越界升级故障、恢复、`invalid_cnt` 清零 |
| `test_fusion` | `fusion.c` | 单源 55/封顶 60、双源 80、窗口过期定案、定案后清空 |
| `test_sensor_calib` | `sensor_calib.c` | 温漂最小二乘拟合、温差不足、补偿量 |
| `test_imu_math` | `domain/sensors/imu_math.c` | 重力分离收敛、冲击响应、滑窗 RMS/峰值 |
| `test_warn_policy` | `domain/alarm/warn_policy.c` | 三级声光节拍、静音自动解除、手动锁定让位 |
| `test_warn_rule` | `domain/warn_rule.c` | 双阈值 + 防抖、LT/GT 方向、中断清零、周期性重触发 |
| `test_json_util` | `domain/json_util.c` | 紧凑/带空白 JSON 取值、缺键、截断、转义、前缀不误匹配 |

## 不在范围内

- `cam_feed`：依赖 LVGL canvas 与 V4L2，仅板端验证；
- `detector`：依赖 ncnn 模型与 C++ 运行时；
- MQTT/OTA 网络路径：板端 + 云端联调验证（`docs/verification-guide.md`）。

## 说明

- 测试使用 `/tmp/gw-*` 作为临时目录，运行前会自行清理；
- 日志服务未初始化时 `log_write` 直接输出到 stderr，不影响用例判断。
