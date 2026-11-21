# Edge AI Security Gateway v2 功能完善实施方案

> 版本：v1.0（2026-11-06）
> 范围：在不更换 Linux 4.1.15 内核的前提下，补齐可靠性、传感器数据处理、网络运维、OTA、安全与可测试性能力。
> 适用仓库：`Edge-AI-Gateway/`（`core/` 驱动 + `core/lvgl/` 应用 + `cloud/` 云端服务）。
> 内核升级：本方案暂不实施，但所有新增接口按“可迁移”原则设计，详见第 9 章。

---

## 0. 结论摘要

- 现有项目是 **v1 可运行原型**：两级 AI 检测、LVGL UI、8 个驱动、抓拍、MQTT 上报均已跑通。
- v1 的主要短板集中在五处：**传感器数据质量、断网与安全、线程同步、配置/日志/OTA、可测试性**。
- v2 的目标不是“加更多功能”，而是把 v1 从“能演示”提升到“**可长期运行、可远程运维、可量化验证**”。
- 实施顺序：**P0 配置与传感器 → P0 断网缓存 → P0 线程重构 → P1 OTA/看门狗/日志 → P2 安全与设备接入**。
- 预计工作量：**5~6 周（兼职）**，按里程碑 M1~M5 交付。

---

## 1. 背景与目标

### 1.1 现状（v1）

| 维度 | 现状 |
|---|---|
| 硬件 | i.MX6ULL Cortex-A7 528MHz，7 寸 1024×600，GT9147，OV5640，TF 卡，百兆网 |
| 系统 | Linux 4.1.15 + Buildroot，NFS 开发模式 |
| 应用 | LVGL v9.2.2 单进程；主线程 + 采集 + cam_feed + detector(nice+10) + MQTT(detach) |
| 检测 | 帧差粗判 → ncnn SCRFD 本地初判 → 云端 YOLOv8n + face_recognition 复核 |
| 驱动 | led / key_event / gpio_event_capture / beep_pwm / ap3216c / icm20608 / key_input / gt9147 |
| 网络 | MQTT 3.1.1 QoS0 短连接；HTTP 明文云端复核 |
| 存储 | TF 卡 BMP 抓拍，满卡清理 |
| 配置 | **仅内存，重启丢失** |
| 运维 | **无 OTA、无看门狗、无日志轮转、无远程配置** |

### 1.2 五个面试关注点与 v1 差距

| # | 关注点 | v1 现状 | 差距 |
|---|---|---|---|
| 1 | 传感器精度、漂移校准、异常剔除 | 只读 IIO raw；无校准、无滤波、无异常剔除；IMU 5Hz、光感 2Hz | 🔴 大 |
| 2 | 断网、认证加密、设备接入 | 断网事件直接丢；MQTT/HTTP 全明文无鉴权；固定 client_id、固定 topic | 🔴 大 |
| 3 | 采样频率、滤波、峰值检测、运动伪影、异常判定 | 摄像头帧差 80×50、阈值+2 帧防抖；传感器只有阈值+debounce | 🔴 大 |
| 4 | 任务划分、信号量/队列/互斥/锁、优先级反转、栈溢出、竞态 | 架构方向正确，但存在 volatile 标志、单槽覆盖、每事件建线程、无优雅退出 | 🟡 中 |
| 5 | 断线重连、缓存重发、在线状态、远程配置、OTA、异常恢复、功耗 | 基本全部缺失，仅熄屏/背光 | 🔴 大 |

### 1.3 v2 目标

1. **数据可信**：传感器有采样率、校准、滤波、异常剔除和实测指标。
2. **断网可用**：本地业务不依赖云端；事件本地缓存、恢复补传、幂等去重。
3. **可运维**：设备身份、在线状态、远程配置、应用 OTA、看门狗、日志轮转。
4. **可验证**：每个能力有明确验收标准、故障注入用例和性能数据。
5. **可迁移**：新增接口不绑死 4.1.15 私有实现，为后续内核升级保留空间。

### 1.4 设计原则

- **本地优先**：核心监测、告警、存储不依赖网络。
- **标准接口优先**：POSIX、V4L2、IIO、input、标准 `/dev/watchdog`。
- **有界资源**：所有队列、缓存、日志有容量上限，满时丢低优先级并计数。
- **单写者原则**：共享数据尽量单写多读，跨线程用队列/双缓冲/原子。
- **UI 线程不阻塞**：网络、推理、存储、OTA 不在 LVGL 主线程执行。
- **可回退**：配置、OTA、内核迁移都必须有回退路径。

---

## 2. 现状评估矩阵

| 能力 | v1 | v2 目标 | 优先级 |
|---|---|---|---|
| 配置持久化 | ❌ | JSON + 原子写 + 版本/CRC | P0 |
| 传感器校准 | ❌ | 零偏/六面/温补 | P0 |
| 传感器滤波 | ❌ | EMA + 中值 + 限幅 | P0 |
| 异常剔除 | ❌ | 范围/变化率/3σ/连续无效 | P0 |
| 峰值检测 | ❌ | 窗口 + 双阈值 + 最小间隔 | P0 |
| 摄像头误报抑制 | ⚠️ | 背景建模 + 形态学 + 连通域 + 光照突变 | P0 |
| 线程同步 | ⚠️ | 原子/有界队列/常驻网络线程 | P0 |
| 优雅退出 | ❌ | 信号 + quit + join | P0 |
| 断网缓存补传 | ❌ | JSONL 队列 + 游标 + 幂等 ID | P0 |
| MQTT 重连 | ❌ | 状态机 + 指数退避 + LWT | P1 |
| 设备身份 | ❌ | MAC/OTP 派生 + topic 分级 | P1 |
| 在线状态 | ⚠️ | 心跳 + LWT + 云端影子 | P1 |
| 远程配置 | ❌ | MQTT 下行 + ACK + 持久化 | P1 |
| 应用 OTA | ❌ | 双目录 + 签名 + 回滚 | P1 |
| 看门狗 | ❌ | /dev/watchdog + 健康条件 | P1 |
| 日志 | ❌ | 分级 + 轮转 + 异步落盘 | P1 |
| 安全 | ❌ | TLS/mTLS + 云端鉴权 | P2 |
| 设备接入与管理 | ❌ | 注册/心跳/命令/区域联防 | P2 |
| 功耗 | ⚠️ | 动态帧率 + governor + 实测 | P2 |
| 系统级 OTA | ⏸ 暂不做（单列 E4） | A/B 分区，需改分区表 | P4 |

---

## 3. 目标架构

### 3.1 分层

```
┌──────────────────────────────────────────────────────────────┐
│ UI 层（LVGL，仅主线程）                                        │
│  ui_home / ui_widgets / ui_events                              │
├──────────────────────────────────────────────────────────────┤
│ 业务层                                                         │
│  event_bus（事件总线）· alarm_fsm（告警状态机）· rule_engine    │
│  ota_mgr · config_mgr · health_mgr                             │
├──────────────────────────────────────────────────────────────┤
│ 数据处理层                                                     │
│  sensor_mgr（采样/校准/滤波/异常）· vision（帧差/背景/连通域）  │
│  fusion（多源时间窗关联）                                       │
├──────────────────────────────────────────────────────────────┤
│ 服务层（常驻线程）                                              │
│  net_svc（MQTT/HTTP/重连/队列）· storage_svc（异步落盘）        │
│  log_svc（异步日志）· ota_svc（下载/校验/安装）                 │
├──────────────────────────────────────────────────────────────┤
│ HAL / 设备抽象                                                  │
│  sensor_hal（IIO/POSIX）· camera_hal（V4L2）· gpio_hal          │
│  net_hal（socket/TLS）· storage_hal（文件系统）                 │
├──────────────────────────────────────────────────────────────┤
│ Linux 4.1.15 + 自研驱动 + V4L2/IIO/input                        │
└──────────────────────────────────────────────────────────────┘
```

### 3.2 线程模型（v2）

| 线程 | 职责 | 优先级 | 通信 |
|---|---|---|---|
| main / UI | LVGL 主循环、消费事件队列 | 默认 | 只读队列/双缓冲 |
| sensor | IIO/V4L2 采集、校准、滤波 | 默认 | 写 sensor 队列 |
| camera | V4L2 取帧、转换、帧差/背景 | 默认 | 双缓冲 + 原子索引 |
| detector | ncnn + 云端复核 | nice+10 | 读帧快照、写结论队列 |
| event | 事件总线分发、规则判断 | 默认 | 消费事件队列 |
| net | MQTT/HTTP、重连、补传 | 默认 | 消费发送队列 |
| storage | 异步写抓拍/事件/日志 | nice+5 | 消费存储队列 |
| health | 心跳、看门狗、自检 | 默认 | 原子标志 |

**原则**：

- 网络/存储/推理**不在 UI 线程**；
- 所有跨线程数据用**有界队列**或**双缓冲 + 原子索引**；
- 不使用 `pthread_detach` 处理业务消息；
- 锁内不做 IO、推理、`lv_*` 调用；
- 线程退出统一由 `app_shutdown()` 唤醒并 `join`。

### 3.3 新增模块清单

| 模块 | 文件建议 | 职责 |
|---|---|---|
| `config_mgr` | `src/config_mgr.c/h` | JSON 配置读写、校验、原子替换、默认值 |
| `sensor_mgr` | `src/sensor_mgr.c/h` | 统一采样、校准、滤波、异常标记 |
| `sensor_calib` | `src/sensor_calib.c/h` | 零偏/六面/温补参数计算与存储 |
| `event_bus` | `src/event_bus.c/h` | 有界事件队列、订阅/分发、优先级 |
| `alarm_fsm` | `src/alarm_fsm.c/h` | NORMAL/PENDING/ACTIVE/MUTED/RECOVERY |
| `fusion` | `src/fusion.c/h` | 多源时间窗关联、置信度评分 |
| `net_svc` | `src/net/net_svc.c/h` | 连接状态机、重连、发送队列 |
| `mqtt_client` | `src/net/mqtt_client.c/h` | 常驻 MQTT（替换短连接） |
| `event_store` | `src/storage/event_store.c/h` | JSONL 事件缓存/游标/补传 |
| `log_svc` | `src/log/log_svc.c/h` | 分级日志、轮转、异步落盘 |
| `ota_mgr` | `src/ota/ota_mgr.c/h` | 下载、校验、安装、回滚 |
| `health_mgr` | `src/health/health_mgr.c/h` | 心跳、看门狗、自检 |
| `device_id` | `src/common/device_id.c/h` | MAC/OTP 派生唯一 ID |

> 现有 `dev_bridge` 保留为“外设桥接 + UI 适配层”，逐步把采集/判定逻辑下沉到 `sensor_mgr` / `event_bus`，避免一次性大重构。

---

## 4. 任务总览

| 编号 | 任务 | 优先级 | 预计 | 依赖 |
|---|---|---|---|---|
| T1 | 配置持久化 | P0 | 1~2 天 | 无 |
| T2 | 日志系统 | P0 | 2~3 天 | T1 |
| T3 | 传感器校准/滤波/异常剔除 | P0 | 5~7 天 | T1、T2 |
| T4 | 摄像头误报抑制 | P0 | 3~5 天 | T2 |
| T5 | 线程模型与同步重构 | P0 | 3~5 天 | T2 |
| T6 | 断网缓存与补传 | P0 | 5~7 天 | T1、T5 |
| T7 | MQTT 重连/设备身份/在线状态 | P1 | 3~5 天 | T6 |
| T8 | 看门狗与健康检查 | P1 | 2~3 天 | T5 |
| T9 | 应用 OTA | P1 | 7~10 天 | T1、T2、T8 |
| T10 | 远程配置 | P1 | 3~4 天 | T7、T1 |
| T11 | 安全（TLS/mTLS/鉴权） | P2 | 5~7 天 | T7、T9 |
| T12 | 命令 ACK + 超时重试 | P2 | 2~3 天 | T7、T11 |
| T13 | 功耗优化 | P2 | 3~5 天 | T4、T5 |
| T14 | 测试与报告 | 全程 | 持续 | 全部 |

---

## 5. P0 任务详细设计

### 5.1 T1 配置持久化

> **实施状态：已完成（2026-11-06）**
> 新增 `src/config_mgr.c/h`（最小 JSON + 原子写），已接入 `main.c`（启动加载）、
> `dev_bridge.c`（预警阈值恢复/保存）、`ui_home.c`（亮度/定时布防/云端复核/帧差参数
> 的恢复与 1s 批量落盘）。运行时配置默认写入 `/opt/edge-gateway/config/gateway.json`，
> 不可写时依次回退 `/etc/edge-gateway/`、`./gateway.json`。

#### 现状问题

- `ui_home.c` 的设置项、`dev_bridge.c` 的 `g_cfg` 都只在内存；
- 重启后布防策略、阈值、云端复核开关等全部恢复默认；
- 面试官现场改设置再重启即可发现。

#### 目标

- 所有用户可配置项落盘；
- 掉电不损坏；
- 非法/损坏配置回退默认并记录错误；
- 配置变更可热生效（采集线程下一轮读取）。

#### 方案

1. 配置文件：`/opt/edge-gateway/config/gateway.json`（开发期可用 `./config/gateway.json`）。
2. 写入流程：
   ```
   snprintf(tmp, "%s.tmp", path);
   fopen(tmp) → 写 JSON → fflush → fsync(fileno) → fclose
   rename(tmp, path)        // 原子替换
   fsync(dirfd)             // 目录项持久化
   ```
3. 读取流程：解析 → schema 校验 → 范围裁剪 → 失败则用默认值 + 记录 `CONFIG_INVALID` 事件。
4. 配置版本：`version` 字段；低版本自动迁移，高版本拒绝并回退。
5. JSON 库：优先 **cJSON**（轻量、单文件）；不引入重量级依赖。
6. 并发：配置用**双缓冲 + 读写锁**；写者构造新副本后原子切换指针，采集线程读旧副本不阻塞。

#### 配置 Schema（v2）

```json
{
  "version": 2,
  "device": {
    "id": "gw-001122334455",
    "name": "Edge Gateway"
  },
  "camera": {
    "enabled": true,
    "device": "/dev/video0",
    "width": 640,
    "height": 480,
    "fps": 30,
    "flip_v": 1,
    "flip_h": 1
  },
  "motion": {
    "enabled": true,
    "interval": 1,
    "diff": 24,
    "ratio": 5,
    "debounce": 2,
    "cooldown_ms": 60000,
    "bg_alpha": 0.05,
    "min_area": 20,
    "light_jump": 15
  },
  "sensors": {
    "ap3216c": { "enabled": true, "period_ms": 500, "als_range": 0 },
    "icm20608": { "enabled": true, "period_ms": 20, "accel_g": 16, "gyro_dps": 2000, "dlpf_hz": 20 }
  },
  "warn": {
    "debounce": 3,
    "cooldown_sec": 30,
    "zones": [
      { "src": 0, "channel": 0, "threshold": 0, "cmp": 0, "enabled": 0 },
      { "src": 0, "channel": 0, "threshold": 0, "cmp": 0, "enabled": 0 },
      { "src": 0, "channel": 0, "threshold": 0, "cmp": 0, "enabled": 0 },
      { "src": 0, "channel": 0, "threshold": 0, "cmp": 0, "enabled": 0 }
    ]
  },
  "alarm": { "led": true, "buzzer": true },
  "network": {
    "mqtt_enabled": true,
    "broker": "192.168.3.100",
    "port": 1883,
    "tls": false,
    "username": "",
    "keepalive": 60,
    "queue_max": 1000
  },
  "cloud": {
    "enabled": true,
    "server": "192.168.3.26:8000",
    "timeout_ms": 2500,
    "tls": false
  },
  "ota": { "enabled": true, "server": "", "auto_check": false },
  "log": { "level": "info", "max_size_kb": 256, "max_files": 4 },
  "power": { "idle_fps": 5, "active_fps": 30, "screen_off_sec": 600 }
}
```

#### 接口

```c
int  config_init(const char *path);
const struct app_config *config_get(void);          /* 只读快照，双缓冲 */
int  config_update(const struct app_config *cfg);   /* 校验 + 原子落盘 + 切换 */
void config_reset_default(void);
```

#### 验收

- 修改设置 → 重启 → 配置保持；
- 人为写入非法 JSON/越界值 → 启动回退默认并记录事件；
- 掉电测试 100 次（可脚本模拟 `kill -9` + 断电）配置文件不损坏；
- 配置文件权限 600，目录 700。

---

### 5.2 T2 日志系统

> **实施状态：代码已完成（2026-11-09），待 VM 编译与板端验证**
> 新增 `src/log_svc.c/h`（分级、异步队列、256KB×4 轮转、stderr 同步输出）；
> 已接入 `main.c`、`config_mgr.c`、`dev_bridge.c`、`cam_feed.c`、`detector.cpp`、
> `lvgl_port.c`、`storage_mgr.c`、`ui_home.c`、`sim_main.c` 的日志点。
> 验证命令见 `docs/verification-guide.md` 第 2 章。

#### 目标

- 分级（DEBUG/INFO/WARN/ERROR/FATAL）；
- 模块、时间、线程 ID、可选错误码；
- 单文件 256KB × 4 轮转；
- 高频采集线程只写内存环形缓冲，日志线程异步落盘；
- 关键事件（告警、断网、OTA、故障）必须可追溯。

#### 接口

```c
typedef enum { LOG_DEBUG, LOG_INFO, LOG_WARN, LOG_ERROR, LOG_FATAL } log_level_t;
void log_init(const char *dir, log_level_t level, int max_kb, int max_files);
void log_write(log_level_t lv, const char *module, const char *fmt, ...);
#define LOGI(mod, ...) log_write(LOG_INFO, mod, __VA_ARGS__)
```

#### 实现要点

- 环形缓冲 32KB，写满丢 DEBUG/INFO 并计数；
- 消费者线程每 200ms 或半满时批量写文件；
- `O_APPEND` + `fsync` 按批，不在每条日志 `fsync`；
- 轮转：`gateway.log` → `.1` → `.2` → `.3`，删除最旧；
- 板端同时保留 `dmesg`/串口关键打印，便于现场排障。

#### 验收

- 日志文件不超过配额；
- 连续运行 24h 无日志丢失导致的阻塞；
- `kill -9` 后最多丢 200ms 日志；
- 日志中能定位一次完整告警链路（采集→判定→声光→上报）。

---

### 5.3 T3 传感器校准、滤波、异常剔除

> **实施状态：T3-A 已通过；T3-C 已通过；T3-B 驱动 buffer 已验证，应用层已切换到 `/dev/iio:deviceX` 待运行验证**
> T3-A 新增 `src/sensor_mgr.c/h`：范围/变化率/3σ 剔除、EMA+中值滤波、
> 连续无效升级故障、STALE 检测；已接入 `dev_bridge.c`，IMU 采样提升到 10Hz，
> 并补充 IIO 设备 rmmod/insmod 重连（1s 轮询）。
> T3-C 新增 `src/sensor_calib.c/h` + `gateway.json` 的 `calib` 节，
> 支持零偏/比例/温度偏置和 `GATEWAY_CALIB_ONCE=1` 上电采集。
> 验证命令见 `docs/verification-guide.md` 第 3 章。

#### 5.3.1 现状与问题

| 项 | 现状 | 问题 |
|---|---|---|
| IMU 采样 | 应用 200ms 读一次（5Hz） | 芯片 1kHz + DLPF 20Hz，5Hz 严重欠采样；振动检测无效 |
| IMU 校准 | 无 | ±16G 零偏、温漂、安装倾斜未处理 |
| 光感采样 | 500ms（2Hz） | 对快速遮挡/环境光变化响应慢 |
| 滤波 | 仅芯片 DLPF | 无软件滤波、无脉冲抑制 |
| 异常 | 读失败静默跳过 | 无故障事件、无降级策略 |
| 峰值 | 无 | 只有阈值 + debounce |

#### 5.3.2 采样策略

| 传感器 | v1 | v2 目标 | 说明 |
|---|---|---|---|
| AP3216C | 2Hz | 5~10Hz | 光照/接近变化不需要更高 |
| ICM20608 | 5Hz | 50~200Hz（振动）/ 20Hz（姿态） | 按业务模式切换 |
| 门磁/PIR | 中断 | 中断 + 时间戳 | gpio_event_capture 已有 |

**IMU 高频采样两条路**：

1. **IIO buffer + trigger**（推荐）：驱动增加 `iio_triggered_buffer_setup` 和 hrtimer trigger，应用读 `/dev/iio:deviceX` 二进制；
2. **sysfs 轮询**（降级）：50Hz 以内可用，但每次 `open/lseek/read` 开销大，需实测 CPU。

> 4.1.15 的 IIO 框架支持 buffer；自研 `icm20608.c` 当前只实现 `read_raw`，需要补 buffer 支持。此项涉及驱动改动，放在 T3 的第一阶段。

#### 5.3.3 校准

1. **陀螺零偏**：上电静止 2s，采集 N 个样本取均值，写入 `calib.json`；运行中零速检测（ZUPT）缓慢更新。
2. **加速度六面校准**：六个朝向各静止 2s，求每轴 offset/scale：
   ```
   offset = (raw_pos + raw_neg) / 2
   scale  = (raw_pos - raw_neg) / (2 * 1g)
   ```
3. **温度补偿**：记录不同温度下的零偏，线性/分段拟合；至少做一次温升实验给出系数。
4. **安装姿态**：记录重力方向，计算安装倾角；告警判定使用“去重力后的动态加速度”，避免倾斜误报。
5. **存储**：`/opt/edge-gateway/config/calib.json`，带 `version`、`crc`、时间戳；校准失败保留旧值。

#### 5.3.4 滤波

| 场景 | 方法 | 参数建议 |
|---|---|---|
| 光照 | 一阶低通/滑动平均 | α=0.2，窗口 5 |
| 加速度/角速度 | EMA + 中值（N=5） | α=0.1~0.3 |
| 脉冲噪声 | 中值滤波 | N=5 |
| 振动能量 | 滑动窗口 RMS | 窗口 100~500ms |
| 姿态 | 互补滤波/卡尔曼 | 后续可选 |

#### 5.3.5 峰值检测

```c
typedef struct {
    float window[PEAK_WIN];   /* 1s 窗口 */
    int   idx, count;
    float last_peak;
    uint64_t last_peak_ms;
} peak_det_t;

/* 双阈值 + 回差 + 最小间隔 */
int peak_update(peak_det_t *p, float v, uint64_t now_ms,
                float th_high, float th_low,
                uint32_t min_interval_ms, float *out_peak);
```

- 上升沿超过 `th_high` 记候选，回落到 `th_low` 以下确认；
- 两次峰值间隔小于 `min_interval_ms` 忽略（refractory）；
- 输出峰值 + 持续时间 + 能量。

#### 5.3.6 异常剔除

| 检查 | 规则 | 动作 |
|---|---|---|
| 物理范围 | 加速度 > ±16G、光照越界、PS 越界 | 标记 `OUT_OF_RANGE`，丢弃 |
| 变化率 | 相邻样本差 > 物理上限 | 标记 `SPIKE`，丢弃 |
| 统计离群 | 偏离滑动均值 > 3σ 或 MAD | 标记 `OUTLIER`，降权 |
| 通信失败 | read 返回 <0 / 超时 | 计数 +1，连续 3 次 → `COMM_ERR` |
| 数据陈旧 | 超过 3×周期无新样本 | `STALE`，UI 显示故障 |
| 饱和 | 连续样本接近量程上限 | `SATURATED`，提示更换量程 |

**故障事件**：连续无效达到阈值 → 发布 `EV_SENSOR_ERROR`（含 sensor_id、错误码、持续时间），UI 时间轴 + 日志 + MQTT 上报；**不退出应用**。

#### 5.3.7 数据结构

```c
typedef enum {
    SENSOR_OK = 0,
    SENSOR_STALE,
    SENSOR_OUT_OF_RANGE,
    SENSOR_SPIKE,
    SENSOR_OUTLIER,
    SENSOR_COMM_ERR,
    SENSOR_SATURATED
} sensor_status_t;

typedef struct {
    uint32_t sensor_id;      /* AP3216C / ICM20608 / ... */
    uint16_t type;           /* accel / gyro / lux / proximity */
    uint64_t ts_ms;          /* monotonic */
    float    v[3];           /* 物理量，已校准 */
    float    raw[3];         /* 原始值，便于排障 */
    uint16_t status;         /* sensor_status_t */
    uint16_t seq;
} sensor_sample_t;
```

#### 5.3.8 验收

- 静止 10 分钟：给出零偏、噪声 RMS、峰峰值；
- 温升实验：给出温度-零偏曲线；
- 六面校准：误差 < 2%；
- 异常注入：断线、越界、跳变、连续无效，均能上报故障；
- 50Hz 采样连续 24h，丢样率 < 0.1%；
- 与 v1 对比：同一场景误报次数下降（需记录）。

---

### 5.4 T4 摄像头误报抑制

> **实施状态：代码已完成（2026-11-09），待摄像头接线后板端验证**
> `cam_feed.c` 已改为背景建模 + 光照突变抑制 + 3×3 开运算 + 连通域面积过滤；
> 新增 `bg_alpha/min_area/light_jump` 三个可调参数。验证命令见
> `docs/verification-guide.md` 3.12。

#### 现状

- 80×50 灰度帧差，`diff > 24` 占比 > 5%，连续 2 次；
- 灯光开关、自动曝光、窗帘/风扇会误报；
- 无 ROI、无背景建模、无形态学、无连通域。

#### 方案

1. **全局光照突变检测**：当前帧平均亮度与背景均值差 > `light_jump`（默认 15%）→ 判定为光照事件，重置背景，不产生运动命中。
2. **背景建模**：滑动平均
   ```
   bg = bg * (1 - α) + cur * α     （α=0.05）
   ```
3. **前景提取**：`|cur - bg| > diff`，得到 80×50 二值图。
4. **形态学**：3×3 开运算（先腐蚀后膨胀），去除孤立噪点。
5. **连通域**：4 邻域标记；过滤面积 < `min_area`（默认 20 个降采样像素）、长宽比异常的块。
6. **运动能量**：前景像素占比 + 持续时间；连续 2~3 帧才命中。
7. **ROI**：可配置矩形区域（如只监控门口），减少无效区域。
8. **阴影抑制**（可选）：HSV 亮度比 + 色度差；单核 528MHz 下先用亮度差 + 形态学即可。
9. **冷却**：沿用 `cooldown_ms`，避免持续运动刷屏。

#### 接口

保持 `cam_feed.h` 不变，内部替换 `motion_detect()`；新增配置项通过 `config_mgr` 读取。

#### 验收

- 开/关灯 20 次：误报 0 次（允许 1 次）；
- 风扇/窗帘持续运动：不产生告警；
- 人走过：命中率 > 95%；
- 80×50 处理耗时 < 2ms/帧（单核）。

---

### 5.5 T5 线程模型与同步重构

> **实施状态：T5-A（事件总线 + 告警状态机）代码已完成（2026-11-09），待验证**
> 新增 `src/event_bus.c/h`、`src/alarm_fsm.c/h`，已接入 `dev_bridge` 的告警
> 触发/消警与 `ui_home` 的人工消警。
> **T5-B（atomic/有界队列/优雅退出/栈大小/锁顺序）尚未做**，留到下一阶段。

#### 问题清单

| 问题 | 位置 | 影响 |
|---|---|---|
| `volatile` 标志非原子 | `detector.cpp`、`cam_feed.c` | 数据竞争、编译器重排 |
| detector 单槽覆盖 | `detector_submit/report` | 连续事件丢帧/结论覆盖 |
| `s_result_name` 非原子 | `detector_poll_result` | 字符串撕裂 |
| 每事件建线程 | `mqtt_hub.c` | 线程/fd/内存耗尽 |
| 无优雅退出 | `main.c` | 资源泄漏、测试困难 |
| 无栈大小控制 | 所有 `pthread_create` | 栈溢出不可量化 |
| 无锁顺序文档 | `dev_bridge.c` | 未来嵌套锁死锁风险 |

#### 方案

1. **原子**：`stdatomic.h`（C11）或 `__atomic_*` 内建；标志用 `atomic_int`，指针用 `atomic_store/load`。
2. **队列**：实现通用有界环形队列 `ring_q_t`（固定元素大小）：
   ```c
   typedef struct {
       uint8_t *buf;
       size_t   elem_size, cap, head, tail, count;
       pthread_mutex_t lock;
       pthread_cond_t  not_empty, not_full;
       uint32_t drop_cnt;
   } ring_q_t;
   ```
   - 队列满策略：高优先级事件覆盖低优先级，或丢最旧并计数；禁止无限阻塞 UI。
3. **detector**：
   - 帧队列深度 1（只保留最新帧），结论队列深度 8；
   - 用 `atomic_exchange` 取帧，避免覆盖；
   - 结论带序号，`poll` 按序消费。
4. **网络**：常驻 `net_svc` 线程 + 发送队列；MQTT 客户端内部维护重连，不再每事件建线程。
5. **退出**：`signal(SIGINT/SIGTERM)` → `g_quit=1` → `sem_post`/`pthread_cond_broadcast` → `join` 所有线程 → 关闭 fd/文件。
6. **栈**：`pthread_attr_setstacksize`（UI 512KB、采集 256KB、detector 512KB、net 256KB）；编译加 `-fstack-usage`，检查单帧最大栈。
7. **锁顺序**：文档化 `config_lock → queue_lock → io_lock`，禁止反向获取；锁内不做 IO。
8. **检测工具**：PC 模拟器跑 `-fsanitize=thread`；板端 `perf`/`ftrace` 观察调度与锁竞争。

#### 验收

- TSan/Helgrind 无数据竞争报告（PC 模拟器）；
- 连续 72h 无线程数增长、无 fd 泄漏；
- `kill -TERM` 后 2s 内退出，所有线程 join；
- 事件风暴（1000 条/秒）不崩溃、队列有界、丢弃计数可见。

---

### 5.6 T6 断网缓存与补传

> **实施状态：代码已完成（2026-11-09），待板端验证**
> 新增 `src/event_store.c/h`（JSONL + 原子重写）并重构 `mqtt_hub.c`
> （常驻线程、有界队列、指数退避、设备 ID、恢复补传）。
> 验证命令见 `docs/verification-guide.md` 3.13。

#### 目标

- 断网期间事件不丢（至少保留高优先级）；
- 网络恢复后自动补传；
- 云端按 `event_id` 幂等去重；
- 缓存有容量上限，满时丢低优先级并记录。

#### 存储选型

| 方案 | 优点 | 缺点 | 结论 |
|---|---|---|---|
| SQLite | 查询方便、事务 | 依赖重、内存/存储开销 | 可选，二期 |
| 单文件 JSONL + 游标 | 轻量、掉电安全、易实现 | 需要自己管理游标/压缩 | **推荐** |
| 每事件一个文件 | 简单 | 小文件多、目录膨胀 | 不推荐 |

#### JSONL 设计

- 路径：`/opt/edge-gateway/data/queue/events-YYYYMMDD.jsonl`
- 每行一个事件 JSON；
- 游标：`queue/cursor.json`（文件 + 偏移 + 最后成功 `event_id`）；
- 发送成功：更新游标；定期删除已确认的旧段；
- 容量：最多 7 天 / 1000 条 / 10MB，超限按优先级淘汰；
- 掉电：批量 `fsync`（每 10 条或 1s）。

#### 事件格式

```json
{
  "event_id": "gw-001122334455-000123-000045",
  "device_id": "gw-001122334455",
  "ts": 1790000000,
  "type": "alarm",
  "level": 2,
  "zone": 0,
  "source": "STRANGER",
  "message": "detected",
  "retry": 0
}
```

- `event_id` = `device_id` + `boot_id` + `seq`，保证幂等；
- 云端按 `event_id` 去重，重复返回 200 但不重复入库。

#### 补传策略

1. 网络上线 → 读取游标 → 按时间顺序批量发送（每批 20 条）；
2. 发送成功 → 更新游标；
3. 失败 → 指数退避（1s/2s/4s…60s）；
4. 高优先级（告警）优先于低优先级（状态/日志）；
5. 补传期间新事件继续入队，保证顺序。

#### 验收

- 拔网线 10 分钟，产生 100 条事件 → 恢复后 60s 内全部补传；
- 重复发送 10 次 → 云端只入库 1 条；
- 缓存满 → 丢最旧低优先级，计数可见；
- 断电重启 → 未发送事件仍在，游标不丢。

---

## 6. P1 任务详细设计

### 6.1 T7 MQTT 重连、设备身份、在线状态

> **实施状态：代码已完成（2026-11-09），待 broker/板端验证**
> T6 已完成重连/设备 ID；T7 补充 Will/LWT、上线消息、`cmd` 订阅和 ACK。
> 验证命令见 `docs/verification-guide.md` 3.17。

#### 设备身份

- 优先读取 SoC unique ID（`/sys/bus/nvmem/devices/*/nvmem` 或 `/proc/device-tree/serial-number`）；
- 回退到 `eth0` MAC（`/sys/class/net/eth0/address`）；
- 生成 `gw-<12 hex>`，首次启动写入 `gateway.json`；
- 所有 topic、事件 ID、云端注册使用该 ID。

#### Topic 设计

```
gateway/{device_id}/status      # online/offline（LWT）
gateway/{device_id}/event       # 事件（QoS1）
gateway/{device_id}/alarm       # 告警（QoS1）
gateway/{device_id}/telemetry   # 传感器/状态（QoS0）
gateway/{device_id}/cmd         # 下行命令（QoS1）
gateway/{device_id}/cmd/ack     # 命令确认
gateway/{device_id}/ota         # OTA 通知
```

#### 连接状态机

```
DISCONNECTED → CONNECTING → CONNECTED → SUBSCRIBING → ONLINE
                    ↑            │
                    └── BACKOFF ←┘（失败/断线）
```

- 指数退避：1s、2s、4s、8s、…、60s + ±20% jitter；
- keepalive 60s，`PINGREQ` 超时 1.5×keepalive 判定断线；
- LWT：`gateway/{id}/status` = `{"online":false,"ts":...}`；
- 上线：发布 `online` + 版本 + 能力；
- 订阅：`cmd`、`ota`。

#### 实现建议

- 用 **mosquitto 客户端库**（libmosquitto）替代手写 MQTT；Buildroot 可带；
- 若坚持零依赖，至少把短连接改为常驻 socket + 重连状态机 + QoS1 PUBACK；
- 网络线程只做收发，业务通过队列交互。

#### 验收

- 断网 5 分钟 → 恢复后 60s 内重连；
- 重连期间事件不丢（T6）；
- 设备离线 90s 内云端显示 offline（LWT）；
- 云端能按 device_id 区分多台设备。

### 6.2 T8 看门狗与健康检查

> **实施状态：已通过板端验证（2026-11-09）**
> 新增 `src/health_mgr.c/h`：单实例（`O_EXCL` + pid 存活检查）、`/dev/watchdog` 30s、
> MAIN/BRIDGE/CAM/NET 心跳、超时停止喂狗。
> 已验证：第二实例被拒绝、看门狗异常复位生效。验证命令见
> `docs/verification-guide.md` 3.14。

#### 看门狗

- `/dev/watchdog`，`WDIOC_SETTIMEOUT` 设 30s；
- 喂狗条件（全部满足）：
  - 主循环心跳 < 5s；
  - camera 线程帧计数增长；
  - sensor 线程心跳 < 3×周期；
  - 存储可写；
  - 事件队列未长时间满。
- 任一条件失败 → 停止喂狗 → 系统复位；
- 启动时记录复位原因（`/sys/class/watchdog/watchdog0/bootstatus` 若支持）。

#### 健康自检

启动时检查：配置、存储挂载、摄像头、模型、网络（可选）；失败降级（禁用该模块）而不是退出；UI 显示故障状态。

#### 验收

- `kill -STOP` 主线程 → 30s 内复位；
- 拔摄像头 → 健康检查标记故障，UI 提示，不重启；
- 存储只读/满 → 降级为内存缓存 + 告警。

### 6.3 T9 应用 OTA

> **实施状态：代码已完成（2026-11-09），待编译/本地模拟/板端验证**
> 新增 `src/ota_mgr.c/h`：包完整性校验、双目录安装、current 原子切换、
> PENDING_CONFIRM/CONFIRMED/ROLLED_BACK 状态机、安装中断回滚。
> 当前签名只检查文件存在，验签留到后续；验证命令见
> `docs/verification-guide.md` 3.15。

#### 目录

```
/opt/edge-gateway/
├── releases/
│   ├── 1.0.0/lvgl_gateway
│   └── 1.1.0/lvgl_gateway
├── current -> releases/1.1.0
├── config/
├── data/
├── logs/
└── ota/
    ├── packages/
    ├── staging/
    └── state.json
```

#### 包格式

```
edge-gateway-v1.1.0/
├── manifest.json
├── payload.tar.gz
├── checksums.sha256
└── signature.sig
```

`manifest.json`：product、hardware、version、minimum_version、package_type、size、sha256、signature_alg。

#### 流程

1. 检查版本（semver 比较）；
2. 下载到 `ota/packages/`（断点续传可选）；
3. SHA-256 校验；
4. 公钥验签（Ed25519 优先，RSA-2048 备选）；
5. 解压到 `ota/staging/`；
6. 检查文件结构、权限、空间；
7. 安装到 `releases/<version>/`；
8. 原子切换 `current`（临时 symlink + rename）；
9. 重启应用；
10. 健康检查（进程、UI、摄像头、配置）；
11. 成功写 `state.json`；失败切回旧版本。

#### 断电恢复

- `state.json` 记录 `phase`、`target_version`、`previous_version`；
- 启动时若 `phase != confirmed` → 回滚；
- 旧版本至少保留 1 个。

#### 验收

必须通过 6 个故障场景：正常、下载中断、哈希错误、签名错误、断电、启动失败回滚。

### 6.4 T10 远程配置

> **实施状态：代码已完成（2026-11-09），待 broker/板端验证**
> 已支持 `set_config` / `set_warn` / `get_config`，ACK 走 `cmd/ack`；
> `dev_bridge` 每 1s 同步配置并刷新 UI。验证命令见 `docs/verification-guide.md` 3.17。

- 下行：`gateway/{id}/cmd`，JSON `{"cmd":"set_config","id":123,"payload":{...}}`；
- 校验：schema + 范围 + 权限；
- 应用：`config_update()` 原子落盘；
- ACK：`cmd/ack` 返回 `{"id":123,"ok":true}` 或错误码；
- 安全：仅接受已认证连接；敏感项（网络/OTA）需二次确认；
- 回滚：应用失败自动恢复上一版配置。

---

## 7. P2 任务概要

### 7.1 T11 安全

- MQTT TLS（8883）+ 用户名密码；进阶 mTLS；
- HTTP 改 HTTPS，云端 `/detect`、`/whitelist/*` 加 Bearer/证书鉴权；
- 证书/密钥权限 600，放 `/etc/ssl/`；
- 时间同步（NTP）保证证书有效期校验；
- 隐私：抓拍访问控制、保留周期、删除接口。

### 7.2 T12 命令 ACK + 超时重试（已裁剪）

**最终范围**：

- 设备端：命令 ACK、id 去重（重试只补 ACK，不重复执行，保护 OTA）；
- 云端：超时重发（默认 2s × 3 次），支持 CLI 与 `POST /cmd`。

**原计划中已舍弃**：设备注册、心跳/能力上报、云端设备表、区域联防、子设备接入。
这些不影响单台网关的现有功能，作为后续扩展方向记录。

### 7.3 T13 功耗

- 动态帧率：无人 5fps、有人 30fps；
- 摄像头按需开关（预览/布防策略）；
- CPU governor：ondemand/conservative；
- 背光自动 + 夜间模式；
- 实测电流：待机、预览、告警、OTA 四种状态。

### 7.4 T14 测试与报告（口径已定稿）

**T14 的交付口径 = 测试与报告，不含系统级 A/B OTA。**

| 类别 | 内容 | 交付物 |
|---|---|---|
| 单元测试 | HMAC、配置、事件缓存、传感器滤波、融合、温漂拟合 | `tests/` + `make test` |
| 板端集成 | 断网补传、MQTT 重连、OTA 安装/回滚、单实例、看门狗 | `docs/verification-guide.md` |
| 故障注入 | 传感器掉线、线程阻塞、坏包/篡改、断电、认证失败 | `docs/fault-injection.md` |
| 性能 | CPU/RAM/FPS/延迟/重连/OTA 耗时 | `docs/performance.md` |
| 稳定性 | 24h 连续运行（D2） | `docs/stability-24h.md`（待做） |

**不在 T14 范围**：系统级 OTA（A/B 分区、U-Boot 启动计数、回滚）。
该项单列为 **E4**，需要改分区表、风险高，暂不做，留待内核升级阶段。

---

## 8. 测试与验收

### 8.1 测试分层

| 层级 | 环境 | 内容 |
|---|---|---|
| 单元测试 | PC | 配置解析、环形队列、滤波、峰值、事件 ID、JSONL 游标 |
| 集成测试 | 板端 | 断网/恢复、断电、满卡、摄像头拔插、传感器失效、OTA |
| 稳定性 | 板端 | 24h / 72h 连续运行 |
| 性能 | 板端 | CPU、RSS、FPS、延迟、重连时间、OTA 时间 |
| 安全 | 板端/PC | 证书校验、越权接口、密钥权限 |

### 8.2 故障注入清单

- 拔网线 10 分钟；
- `kill -9` 应用；
- 断电（拔电源）；
- TF 卡拔插/写满/只读；
- 摄像头拔插/断流；
- 传感器 I2C/SPI 断线（可模拟 read 失败）；
- OTA 包损坏/签名错误/版本不兼容；
- 主线程 `kill -STOP`（看门狗）；
- 时间跳变（NTP/手动改时间）。

### 8.3 性能指标（需实测回填）

| 指标 | 目标 |
|---|---|
| 启动到 UI 可用 | < 8s |
| 预览帧率 | ≥ 25fps |
| 帧差处理 | < 2ms/帧 |
| 本地人脸推理 | < 1s |
| 云端复核 | < 2.5s |
| 断网重连 | < 60s |
| 事件补传 | 100 条 < 60s |
| 72h 内存增长 | < 5% |
| 告警端到端延迟 | < 1.5s（本地） |

---

## 9. 内核升级预留

本方案不换内核，但以下设计保证后续可迁移：

1. **HAL 抽象**：`sensor_hal`、`camera_hal`、`net_hal`、`storage_hal`，应用层不直接依赖私有 ioctl。
2. **标准接口**：IIO（含 buffer）、V4L2、input、`/dev/watchdog`、POSIX socket；避免新增 4.1.15 私有接口。
3. **摄像头**：`cam_feed` 保持“取帧 → RGB565 → 双缓冲”接口不变；内核升级时只替换内部 V4L2 pipeline（`mx6s_capture` → `imx7-media-csi` + media controller）。
4. **驱动**：自研驱动继续使用标准子系统（gpiod、IIO、input、PWM），升级时按 6.x API 适配。
5. **配置/事件/OTA**：与内核无关，可直接复用。
6. **兼容矩阵**：后续内核迁移时，用本方案的验收用例逐项回归。

**已核查的升级候选**（供后续参考，不在本次实施）：

- NXP `lf-6.12.y`（6.12.49 LTS）：有 i.MX6ULL DTS，CSI 走 `imx7-media-csi`（支持 `fsl,imx6ul-csi`），显示走 DRM mxsfb；
- NXP `lf-6.6.y`（6.6.52 LTS）：同样支持；
- 主线 6.12/6.18 LTS：支持 6ULL，但板级 DTS/显示/摄像头需更多自建。

---

## 10. 里程碑与工作量

| 里程碑 | 内容 | 周期 | 交付物 |
|---|---|---|---|
| M1 | T1 配置 + T2 日志 + T3 传感器（应用层） | 1.5 周 | 配置持久化、日志、校准/滤波/异常剔除 + 实测数据 |
| M2 | T4 摄像头 + T5 线程重构 | 1.5 周 | 误报抑制、原子/队列、优雅退出 |
| M3 | T6 断网缓存 + T7 MQTT 重连/设备身份 | 1.5 周 | 事件不丢、可补传、设备可区分 |
| M4 | T8 看门狗 + T9 OTA + T10 远程配置 | 2 周 | 可远程运维、故障自恢复 |
| M5 | 测试、性能、文档、演示 | 1 周 | 测试报告、演示视频、README |
| 合计 | | **5~6 周（兼职）** | |

> 若时间紧，优先完成 M1~M3（约 4 周），已能显著提升面试竞争力；OTA/安全作为加分项。

---

## 11. 风险与回退

| 风险 | 影响 | 缓解 |
|---|---|---|
| 单核 528MHz 性能不足 | 高频采样 + 滤波 + 摄像头 + LVGL 抢 CPU | 先 profiling；降采样/降帧；detector nice+10；必要时 IMU 50Hz |
| 内存不足 | LVGL 256KB + 队列 + 缓冲 | 队列有界；大缓冲静态分配；监控 RSS |
| TF 卡掉电损坏 | 配置/事件丢失 | 原子写 + fsync + 备份 + 只读挂载策略 |
| IIO buffer 驱动改动 | 高频采样受阻 | 先 sysfs 50Hz 验证；buffer 作为增强 |
| OTA 把应用刷坏 | 设备变砖 | 双目录 + 健康检查 + 自动回滚 + 串口恢复 |
| 看门狗误复位 | 演示中断 | 喂狗条件加宽限；调试期可关闭 |
| 改动面过大 | 回归风险 | 分阶段、每阶段可回退；保留 v1 分支 |

**回退策略**：

- 代码：每阶段打 tag/分支；
- 配置：保留 `gateway.json.bak`；
- OTA：保留上一版本；
- 内核：本方案不动，后续单独分支 + SD 卡启动验证。

---

## 12. 面试讲解要点（建议）

每个模块按“**为什么做 → 怎么做 → 怎么验证 → 遇到什么问题**”准备：

1. **传感器**：5Hz 欠采样 → 50Hz + IIO buffer；零偏/六倍/温补；3σ + 变化率 + 连续无效；给出实测数据。
2. **断网**：本地优先 → JSONL 队列 + 游标 + 幂等 ID；拔网线 10 分钟演示补传。
3. **线程**：volatile 的问题 → 原子/有界队列/常驻网络线程；TSan 验证；优雅退出。
4. **OTA**：双目录 + 签名 + 健康检查 + 回滚；6 个故障场景。
5. **安全**：TLS/mTLS + 云端鉴权 + 设备身份；不要只说“加了 TLS”。
6. **内核**：暂不换，但已验证 NXP 6.12 对 6ULL 的 CSI/显示支持，设计了 HAL 抽象和双启动回退方案。

---

## 13. 附录

### 13.1 错误码（建议）

| 码 | 含义 |
|---|---|
| 0 | 成功 |
| 1001 | 配置解析失败 |
| 1002 | 配置写入失败 |
| 2001 | 传感器初始化失败 |
| 2002 | 传感器读取失败 |
| 2003 | 传感器数据越界 |
| 3001 | 存储空间不足 |
| 3002 | 文件写入失败 |
| 4001 | 网络连接失败 |
| 4002 | MQTT 通信失败 |
| 4003 | 云端复核失败 |
| 5001 | OTA 下载失败 |
| 5002 | OTA 校验失败 |
| 5003 | OTA 安装失败 |
| 5004 | OTA 回滚失败 |
| 6001 | UI 初始化失败 |
| 9001 | 系统内部错误 |

### 13.2 事件优先级

| 级别 | 含义 | 示例 |
|---|---|---|
| P0 | 系统级严重故障 | 存储损坏、看门狗即将复位 |
| P1 | 高等级安防告警 | STRANGER/INTRUDER |
| P2 | 普通运动事件 | 画面变动 |
| P3 | 传感器状态 | 光照变化、IMU 数据 |
| P4 | 调试/状态 | 心跳、版本 |

### 13.3 相关文件

- 现有应用入口：`core/lvgl/src/main.c`
- 外设桥接：`core/lvgl/src/dev_bridge.c`
- 摄像头/帧差：`core/lvgl/src/cam_feed.c`
- 精判：`core/lvgl/src/detector.cpp`
- 云端客户端：`core/lvgl/src/cloud_detect.c`
- MQTT：`core/lvgl/src/mqtt_hub.c`
- 存储：`core/lvgl/src/storage_mgr.c`
- 传感器驱动：`core/ap3216c/ap3216c.c`、`core/icm20608/icm20608.c`
- 云端服务：`cloud/server.py`

---

*本方案为实施草案，具体参数和接口在开发中按实测调整。*
