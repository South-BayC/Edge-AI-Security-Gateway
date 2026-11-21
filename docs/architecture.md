# 架构与代码结构（目标态 + 迁移状态）

> 本文是重构的“单一事实来源”。代码目录调整必须先更新本文，再改代码。
> 验证方式：`make lvgl` + `make test` + 板端冒烟。
> 重构范围/等价性/必测清单见 `docs/refactor-summary.md`。

## 1. 分层规则（强制执行）

```
app  →  ui  →  domain  →  infra
                  ↑
                 ports
```

| 层 | 允许依赖 | 禁止 |
|---|---|---|
| `domain/` | C 标准库、`config_types.h`、`event_bus` | LVGL、socket、UI、具体硬件 ioctl |
| `infra/` | OS API（socket/fd/ioctl）、domain 接口 | `ui/*`、`lvgl.h` |
| `ui/` | LVGL、`ui_events.h`、domain 接口 | 直接操作硬件 fd、直接发网络包 |
| `app/` | 所有层（组装根） | 业务实现 |

补充规则：

1. 跨层通信只允许：配置结构体、`event_bus`、显式回调；
2. 每个模块只暴露一个头文件，状态用 `static` 隐藏；
3. 禁止跨模块直接读写全局变量；
4. 新增第三方库必须先登记到 `docs/dependencies.md`。

## 2. 当前目录（Phase 1 完成后）

```
core/lvgl/src/
├─ main.c                 # 组装根
├─ app/mqtt_commands.c/h  # 云端命令分发（配置/OTA/回读 + ACK + 去重）
├─ config_types.h         # 中立配置类型（zone_warn_cfg/dev_config）
├─ zone_types.h           # 中立状态类型（zone_state_t）
├─ domain/zone_state.c/h  # 分区状态（唯一状态源，无 UI/硬件依赖）
├─ domain/sensors/imu_math.c/h # 去重力 + 滑窗 RMS/峰值（纯算法，可单测）
├─ domain/alarm/warn_policy.c/h # 分级声光策略（LED/蜂鸣节拍、静音、手动锁定，可单测）
├─ domain/warn_rule.c/h   # 双阈值 + 防抖判定（AP/IMU 共用，可单测）
├─ domain/json_util.c/h   # 极简 JSON 取值/转义（云端配置/命令，可单测）
├─ infra/hal_io.c/h       # LED / 蜂鸣器 / PWM 设备访问（仅 OS API）
├─ infra/sensor_io.c/h    # IIO sysfs / ICM buffer / KEY0 input 设备访问
├─ infra/sensor_pipeline.c/h # 采集线程 + 校准 + 滤波 + 双阈值 + 诊断快照
├─ infra/mqtt_client.c/h  # MQTT 3.1.1 报文与连接（传输走 mqtt_transport）
├─ infra/net_probe.c/h    # TCP 可达性探测（UI 联网状态用）
├─ config_mgr.c/h         # 配置持久化（仅依赖 config_types）
├─ dev_bridge.c/h         # 硬件/传感器/预警桥接（待继续拆分）
├─ sensor_mgr.c/h         # 滤波/故障状态
├─ sensor_calib.c/h       # 零偏/温漂拟合
├─ cam_feed.c/h           # V4L2 采集 + 转换 + 运动检测
├─ detector.cpp/h         # ncnn SCRFD + 云端复核
├─ cloud_detect.c/h       # 云端精判客户端
├─ alarm_fsm.c/h          # 告警状态机
├─ fusion.c/h             # 多源融合评分
├─ event_bus.c/h          # 领域事件总线
├─ event_store.c/h        # 断网事件缓存
├─ log_svc.c/h            # 日志
├─ health_mgr.c/h         # 看门狗/单实例/重启
├─ thread_util.c/h        # 线程封装
├─ time_sync.c/h          # SNTP
├─ storage_mgr.c/h        # TF 抓拍存储
├─ ota_mgr.c/h            # OTA 状态机
├─ ota_hmac.c/h           # HMAC-SHA256
├─ http_get.c/h           # HTTP 客户端
├─ mqtt_hub.c/h           # MQTT（待拆分）
├─ mqtt_transport.c/h     # TCP/TLS 传输
├─ lvgl_port.c/h          # LVGL 显示/触摸初始化
└─ ui/                    # LVGL 界面
   ├─ ui_home.c           # 主页/检测页/弹窗构建（4490 行，待继续拆面板）
   ├─ ui_events.c/h       # 真实信号接入实现（状态/告警/日志/网络/配置刷新）
   ├─ ui_internal.h       # ui 层内部共享对象与刷新能力（仅 ui/ 可包含）
   ├─ ui_widgets.c/h      # 通用小部件
   ├─ panels/ui_status_bar.c/h # 顶部状态栏（标题/时钟/联网/布防/CPU）
   ├─ panels/ui_timeline.c/h   # 事件时间轴（计数/清空/条目/查看回调）
   ├─ panels/ui_sensor.c/h     # 检测页（5 卡片 + 子页/校准/曲线/系统信息/终端）
   ├─ modals/ui_alarm.c/h      # 告警确认弹窗（长按 1s 消警 + 静音，hooks 注入）
   ├─ modals/ui_settings.c/h   # 设置弹窗（亮度/复核/阈值/定时/系统/关于）
   ├─ modals/ui_album.c/h      # 抓拍相册 + 查看器（含抓拍缓冲与 TF 恢复）
   ├─ ui_canvas.c/h       # canvas 像素绘制（入侵框）
   ├─ ui_time.c/h         # 事件时间字符串（HH:MM）
   ├─ ui_theme.h          # 色板 + 字体（ui 层共用）
   └─ ui_terminal.c/h     # 本地调试终端（fork/pipe + 软键盘）
```

## 3. 已知耦合与迁移计划

| 问题 | 目标 | 阶段 |
|---|---|---|
| `config_mgr.h` 曾依赖 `dev_bridge.h` | 已改为依赖 `config_types.h` | ✅ Phase 1 |
| `detector.cpp` 依赖 `ui/ui_events.h` | 已改为读 `config_get()->cloud_review` | ✅ Phase 1 |
| `state_machine` 内置 `zone_state_t` | 已抽出 `zone_types.h` | ✅ Phase 1 |
| `dev_bridge.c` 直接调用 `ui_events_*` | 已改为发 `event_bus`；UI 50ms dispatch 消费 | ✅ Phase 1b |
| `dev_bridge.c` 依赖 LVGL 定时器 | 已抽出 `dev_bridge_poll()`，由 UI 200ms 定时器调用 | ✅ Phase 1b |
| UI 与状态机各维护一份分区状态 | 已统一到 `domain/zone_state` | ✅ Phase 1b |
| `dev_bridge.c` 单文件 1500 行 | 拆 `hal_io` / `sensor_pipeline` / `warn_policy` | ✅ Phase 2.1 完成（`dev_bridge.c` 1362 → 696 行；设备层 `infra/sensor_io`、采集层 `infra/sensor_pipeline`） |
| `mqtt_hub.c` 单文件 970 行 | 拆 `mqtt_protocol` / `mqtt_client` / `mqtt_commands` | ✅ Phase 2.3（协议→`infra/mqtt_client`，命令→`app/mqtt_commands`；hub 只留队列/重连/上报） |
| `ui_home.c`（原 5000+ 行） | 拆 `ui_events` + `panels/` + `modals/` + `sensors/` | ✅ Phase 2.4 完成（`ui_home.c` 1327 行，只剩布局组装/定时器/事件分发） |
| UI 直接 include fbdev 驱动头 | 已改为 `lvgl_port_vsync_ok()` | ✅ Phase 2.4 |
| UI 直接调用 socket 探测网关 | 已抽到 `infra/net_probe`（`net_probe_tcp`） | ✅ Phase 2.4 |
| 全局 UI 状态副本（brightness 等） | 统一读配置访问器 | ⏳ Phase 3 |

## 4. 依赖与构建

- 第三方依赖目录：`third_party/`（不入 git，清单见 `docs/dependencies.md`）；
- 字库源：`third_party/fonts/NotoSansSC-VF.ttf`（OFL），生成脚本 `tools/fontgen/`；
- 构建配置：`config.mk`（本地路径覆盖，已被 `.gitignore` 忽略）；
- 密钥：`cloud/secrets/`（不入库，由 `cloud/gen_*.sh` 生成）。

## 5. 验收门槛

每个重构提交必须满足：

1. `make lvgl` 无错误；
2. `make test` 全部通过；
3. 板端冒烟：启动 → 摄像头 → 布防/告警 → 远程配置 → 断电重启；
4. 不改变已验证的对外行为（日志关键字、MQTT topic、配置字段）。

## 6. 依赖审计（可复现）

```bash
SRC=core/lvgl/src

# domain 不得依赖 LVGL / UI / socket
grep -rnE '#include .*(lvgl|ui/|sys/socket|netinet|arpa)' $SRC/domain || echo "domain: clean"

# infra 不得依赖 UI / LVGL
grep -rnE '#include .*(ui/|ui_home|lvgl)' $SRC/infra || echo "infra: clean"

# ui 不得直接操作网络/硬件驱动
grep -rnE '#include .*(sys/socket|netinet|arpa|linux/input|sys/ioctl|drivers/)' $SRC/ui || echo "ui: clean"
```

当前结果：三条全部 clean。

已知例外（有意保留，记录在案）：

| 位置 | 原因 |
|---|---|
| `cam_feed.h` 包含 `lvgl/lvgl.h` | 已解除（`cam_feed_start(uint8_t *buf)`，不再需要 canvas/`lv_color_t`） |
| `dev_bridge.c` 仍在 `src/` 根 | 已拆：设备层 `infra/sensor_io` + 采集层 `infra/sensor_pipeline`（`dev_bridge` 仅保留告警/融合/声光/对外 API） |
| `ui_home.c` 仍是单文件 | 面板级拆分（panels/modals/sensors）待做，属可维护性优化，不影响分层 |
