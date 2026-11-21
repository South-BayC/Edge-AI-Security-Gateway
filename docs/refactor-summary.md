# 分层重构总结（2026-11-12）

> 本文记录本轮重构的范围、产物、行为等价性与重构后必测项。
> 架构规则与迁移状态见 `docs/architecture.md`；依赖清单见 `docs/dependencies.md`。

## 1. 目标与完成度

| 目标 | 状态 |
|---|---|
| 完整分层（app / ui / domain / infra），依赖方向单向 | ✅ |
| 跨层只通过 `event_bus` / 配置结构体 / 显式回调 | ✅ |
| 依赖全部在项目内（`third_party/` 不入库但物理存在） | ✅ |
| 清理无用第三方依赖 | ✅ 删除 `ncnn-assets`、`libfacedetection` |
| 中文字库可复现（不依赖 Windows 字体） | ✅ Noto Sans SC + `tools/fontgen/` |
| 密钥与部署路径不入库 | ✅ `cloud/secrets/`、`config.mk` |
| Python 依赖固定版本 | ✅ `cloud/requirements.lock.txt` |
| 纯逻辑模块可单测 | ✅ 10 个用例 |
| `ui_home.c` 面板级拆分 | ✅ 5058 → 1326 行（状态栏/时间轴/告警/设置/详情/相册/检测页/终端全部独立） |
| `dev_bridge.c` 拆分 | ✅ 1362 → 700 行（设备层 `infra/sensor_io`、采集层 `infra/sensor_pipeline`） |
| `cam_feed` 解除 LVGL 依赖 | ✅ `cam_feed_start(uint8_t *buf)`，不再需要 canvas / `lv_color_t` |
| 依赖审计（domain/infra/ui 三条） | ✅ 全部 clean（命令见 `docs/architecture.md` §6） |

当前手写 C 源码规模：**15302 行 / 45 个文件**（不含 LVGL 自身与生成字库）。

分层规模：`domain 384` / `infra 465` / `app 165` / `ui 5555`（其中 `panels 1678`、`modals 1721`），
其余为 `src/` 根部的服务模块（`dev_bridge 1362`、`cam_feed 979`、`config_mgr 947`、`ota_mgr 852`、`mqtt_hub 547` 等）。

## 2. 分层现状

```
main.c（组装根）
  ├─ app/      mqtt_commands          云端命令：配置/OTA/回读 + ACK + 去重
  ├─ ui/       ui_home / ui_terminal / ui_widgets / ui_theme / ui_events.h
  ├─ domain/   zone_state / imu_math / warn_policy / warn_rule / json_util
  └─ infra/    hal_io / mqtt_client / net_probe / mqtt_transport
```

依赖规则（已自动审计通过，命令见 `docs/architecture.md` §6）：

- `domain/` 不含 LVGL、UI、socket；
- `infra/` 不含 UI、LVGL；
- `ui/` 不直接操作 socket / 硬件驱动头；
- `dev_bridge` 不再依赖 UI/LVGL，只发 `event_bus` 事件。

## 3. 新增 / 抽出的模块

| 模块 | 行数 | 职责 | 单测 |
|---|---|---|---|
| `domain/zone_state.c/h` | ~95 | 分区状态唯一源（布防/告警/级别/名称） | — |
| `domain/sensors/imu_math.c/h` | ~90 | 去重力 + 滑窗 RMS/峰值 | `test_imu_math` |
| `domain/alarm/warn_policy.c/h` | ~85 | 三级声光节拍、静音、手动锁定 | `test_warn_policy` |
| `domain/warn_rule.c/h` | ~65 | 双阈值 + 防抖（AP/IMU 共用） | `test_warn_rule` |
| `domain/json_util.c/h` | ~85 | 极简 JSON 取值/转义 | `test_json_util` |
| `infra/hal_io.c/h` | ~140 | LED / 蜂鸣器 / PWM 设备访问 | — |
| `infra/mqtt_client.c/h` | ~284 | MQTT 3.1.1 报文与连接 | — |
| `infra/net_probe.c/h` | ~50 | TCP 可达性探测 | — |
| `infra/sensor_io.c/h` | 198 | IIO sysfs / ICM buffer / KEY0 input 设备访问 | — |
| `infra/sensor_pipeline.c/h` | 716 | 采集线程 + 校准 + 滤波 + 双阈值 + 诊断快照 | — |
| `app/mqtt_commands.c/h` | ~165 | 云端命令分发 + ACK | — |
| `ui/ui_theme.h` | ~40 | 色板 + 字体声明 | — |
| `ui/ui_terminal.c/h` | ~340 | 调试终端（fork/pipe + 软键盘） | — |

单测总数：`test_hmac`、`test_config`、`test_event_store`、`test_sensor_mgr`、`test_fusion`、`test_sensor_calib`、`test_imu_math`、`test_warn_policy`、`test_warn_rule`、`test_json_util`。

`ui/` 拆分结果：

| 模块 | 行数 | 职责 |
|---|---|---|
| `ui/ui_home.c` | 1326 | 主页布局组装、定时器编排、事件分发、hooks 接线 |
| `ui/ui_events.c` | 215 | 真实信号接入（状态/告警/日志/网络/配置刷新） |
| `ui/panels/ui_sensor.c` | 1270 | 检测页 5 卡片 + 子页/校准/曲线/系统信息/终端 |
| `ui/modals/ui_settings.c` | 982 | 设置弹窗（亮度/复核/阈值/定时/关于/重启） |
| `ui/modals/ui_album.c` | 369 | 抓拍相册 + 查看器 + TF 恢复 |
| `ui/ui_terminal.c` | 340 | 调试终端（fork/pipe + 软键盘） |
| `ui/panels/ui_timeline.c` | 260 | 事件时间轴 |
| `ui/ui_events.c` / `ui_status_bar.c` / `ui_detail.c` / `ui_alarm.c` / `ui_canvas.c` / `ui_time.c` / `ui_theme.h` / `ui_internal.h` | 余下 | 事件接入、状态栏、详情、告警、画布工具、时间、主题、内部共享 |

## 4. 行为等价性说明

重构以「搬运 + 不改变已验证行为」为原则，以下细节刻意保持一致：

1. **声光节拍**：高等级 LED 在现实现中为常亮（`period=1` 时 `(phase/2) % 1 == 0`），注释虽写「快闪」，重构未改语义，测试按实际行为锁定。
2. **阈值判定**：达标后计数归零、持续超限周期性重新置位（冷却结束后可立即再触发）。
3. **告警优先级**：告警 > 手动锁定；静音在消警后自动解除。
4. **MQTT**：CONNECT flags/keepalive 60s/遗嘱 topic、CONNACK 校验、PINGREQ 不等响应、>512B 报文整包丢弃、QoS0 payload 上限 768B。
5. **命令幂等**：同 id 只补 ACK 不重复执行；`get_config` 直接回 ACK topic 不记录 id。
6. **JSON 取值**：仍为 `strstr` 定位（同名嵌套键取第一次出现、`"zones"` 不匹配 `"zone"`），已在单测中固化。

## 5. 重构后必测项（板端）

> 功能验证统一在重构完成后进行。以下按「改动影响面」排序，详细步骤引用既有手册。

### 5.1 启动与显示

- [ ] 冷启动无崩溃，fbdev 正常点亮，触摸可用；
- [ ] 状态栏时钟/联网状态刷新；断网后 5s 内显示「离线」，恢复后回「联网」；
- [ ] 息屏 → 触摸/KEY0 唤醒无撕裂（`lvgl_port_vsync_ok` 路径）。

### 5.2 分区状态与告警链路（事件总线重构）

- [ ] 布防/撤防：分区卡状态、状态栏指示同步；
- [ ] 传感器触发：弹窗 + 声光 + 时间轴 + 抓拍；
- [ ] 长按 1s 消警：回布防、声光停、时间轴「报警已消」；
- [ ] 云端复核消警（`ui_events_alarm_auto_clear`）路径；
- [ ] 分级声光：高/中/低三级节奏与实际一致（对照 `test_warn_policy` 期望）；
- [ ] 静音按钮：立即停鸣，消警后自动恢复跟随。

### 5.3 传感器与算法

- [ ] 检测页 5 张卡片数值刷新、故障/恢复状态；
- [ ] 双阈值：仅过预警阈值 → 轻提醒，不告警；
- [ ] 去重力线性加速度（仓库分区）静止≈0，敲击触发；
- [ ] 采样开关冻结数据与曲线。

### 5.4 MQTT / 云端

- [ ] 连接（TLS 8883）成功、上线状态发布、设备影子上报；
- [ ] `set_config` 下发后 UI 立即刷新、`get_config` 回读正确；
- [ ] `ota` 命令仅触发检查（不自动下载）；
- [ ] 断网期间事件入本地缓存，恢复后自动补传且不重复；
- [ ] 同一条命令重复下发（同 id）只执行一次。

### 5.5 终端与系统

- [ ] 检测页终端：执行命令、软键盘输入、清屏、切页自动终止子进程；
- [ ] 重启应用按钮：进程重启后 UI 恢复；
- [ ] KEY0：唤醒 + 时间轴「KEY 触发」。

### 5.6 已知问题（重构前后一致，非回归）

- PWM 蜂鸣器与 OV5640 采集中存在干扰（已降占空比至 30%，硬件整改待定）；
- E3 背光自动/夜间模式未实现（既定范围外）；
- mTLS 未做（单向 TLS 已验收）。

### 5.7 已修复问题（2026-11-12）

- **息屏唤醒后摄像头画面卡住**：`_cam_refresh_cb` 的 VSYNC 门控与 fbdev 恢复形成自锁。
  修复：唤醒后 3s 内强制放行摄像头帧；之后若 VSYNC 仍未恢复，每 10 帧放行 1 帧兜底。

## 6. 临时文件与清理建议

| 路径 | 说明 | 建议 |
|---|---|---|
| `build/cache/lvgl`、`build/lvgl` | 编译缓存/产物 | 可 `make clean` 后重编，不影响源码 |
| `tests/test_*` | 单测可执行文件 | `cd tests && make clean` |
| `tools/fontgen/node_modules` | 字库生成依赖 | 保留（重跑 `generate.sh` 需要）；已 gitignore |
| `third_party/` | ncnn / mbedtls / fonts | 保留（物理依赖，已 gitignore） |
| `cloud/secrets/` | 密钥 | 保留，勿提交 |

## 7. 回滚说明

本工程当前**不是 git 仓库**，重构没有版本回滚点。若需回滚：

1. 重构前应已备份 `core/lvgl/src`、`core/lvgl/Makefile`、`tests/`、`cloud/`、`docs/`；
2. 新增模块均为独立文件，删除即可移除对应能力（需同步恢复被改文件的 include/调用）；
3. 建议后续执行 `git init` 并提交当前基线，避免再次出现「无回滚点」的情况。
