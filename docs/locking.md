# 线程模型与锁顺序（T5-B / B1）

> 目的：明确每个线程的职责、栈大小与锁的获取顺序，避免死锁与栈溢出。
> 代码入口：`core/lvgl/src/thread_util.c`（统一线程创建）。

---

## 1. 线程清单

| 线程 | 创建位置 | 栈大小 | 职责 |
|---|---|---|---|
| main | 进程启动 | 系统默认（8MB 虚拟） | LVGL 渲染、UI 回调、定时器（含 `dev_bridge_poll` / 事件 dispatch）、OTA 重启判断 |
| collect | `dev_bridge_start` | 512KB | KEY0 轮询、IIO sysfs 回退读、阈值判定、写 `g_shm` |
| imu | `dev_bridge_start` | 256KB | `/dev/iio:device1` buffer 读取（100Hz）、校准/滤波、写 `g_shm` |
| cam | `cam_feed_start` | 1MB | V4L2 取帧、YUYV→RGB565、运动检测、帧发布 |
| detector worker | `detector_init` | 2MB | 本地 SCRFD + 云端复核（低优先级 nice+10） |
| mqtt net | `mqtt_hub_start` | 256KB | 连接/重连、发布事件、订阅命令、PING |
| ota | `ota_periodic_start` | 512KB | 定时/命令触发的 OTA 检查（下载、校验、安装） |
| log writer | `log_init` | 128KB | 异步落盘 + 轮转 |
| event_store writer | `event_store_init` | 128KB | `events.jsonl` 原子重写 |
| time sync | `time_sync_start` | 128KB | SNTP 取时（启动 + 6h） |

> 栈大小是“够用上限”，不是占用量；虚拟内存不预占物理页。

---

## 2. 锁清单（均为非递归 mutex）

| 模块 | 锁 | 保护对象 |
|---|---|---|
| `config_mgr` | `s_lock` | 配置快照 |
| `health_mgr` | `s_lock` | 各线程心跳时间戳 |
| `log_svc` | `s_lock` | 日志队列/级别 |
| `event_store` | `s_lock` | 事件行数组/脏标志 |
| `mqtt_hub` | `s_lock` | 发送队列 |
| `dev_bridge` | `g_shm.lock` | 共享采集状态（KEY/ALS/IMU/标志） |
| `sensor_mgr` | `g_lock` | 各通道滤波/状态 |
| `cam_feed` | `s_frame_lock` | 双缓冲索引 + 帧拷贝 |
| `detector` | `s_job_lock` / `s_result_lock` | 任务队列 / 结论交接 |
| `ota_mgr` | `s_ota_lock` | OTA 线程状态/URL |
| `time_sync` | `s_lock` | 同步线程状态 |

---

## 3. 锁顺序规则

**唯一允许的嵌套链（只能从左到右）：**

```
mqtt_hub.s_lock  →  health_mgr.s_lock  →  log_svc.s_lock
```

- 来源：网络线程在空闲等待循环中持有 `mqtt.s_lock` 时调用 `health_beat()`；
  `health_tick()` 持有 `health.s_lock` 时通过 `LOGE` 进入 `log_push()`。
- 其它模块的锁**都是叶子锁**：持有时不得再获取另一把模块锁。

**具体约定：**

1. 禁止反向获取（例如持有 `log_svc.s_lock` 时不得调用 `health_beat` 或 `mqtt_publish`）；
2. `config_update()/config_flush()` 内部只取 `config.s_lock`，调用方不得在持锁状态下调用；
3. `handle_cmd()`（MQTT 线程）在**不持有** `mqtt.s_lock` 的情况下调用 `config_update()` / `ota_request_check()`；
4. `cam_feed` 的 `s_frame_lock` 只覆盖“索引发布 + 428KB memcpy”，不得在锁内做日志/网络/分配；
5. `detector` 的两个锁互不嵌套：`s_job_lock` 只保护队列，`s_result_lock` 只保护结论；
6. 回调（`alarm_fsm` / `event_bus` / UI）统一在主线程执行，回调内不做阻塞 I/O；
7. LVGL API 只在主线程调用，工作线程只写共享数据 + 发布标志。

**检查方式：**

- 代码审查：搜索每个锁的 `lock` 到 `unlock` 区间，确认没有跨模块加锁；
- 需要更强保证时，在 PC 模拟器上用 `valgrind --tool=helgrind` 跑一遍 UI 交互（板端不支持）。

---

## 4. 线程生命周期

```
main
 ├─ log_init        → log writer（join: log_close）
 ├─ mqtt_hub_start  → net（join: mqtt_hub_stop → event_store_close）
 ├─ time_sync_start → sntp（join: time_sync_stop）
 ├─ ota_periodic_start → ota（join: ota_periodic_stop）
 ├─ dev_bridge_start → imu + collect（join: dev_bridge_stop）
 │                     └─ detector_init → worker（join: detector_stop）
 └─ cam_feed_start  → cam（join: cam_feed_stop）
```

退出顺序（`main.c`）：

```
ota_periodic_stop → time_sync_stop → dev_bridge_stop → cam_feed_stop
→ detector_stop → mqtt_hub_stop → health_shutdown → log_close
```

实测：`killall lvgl_gateway` 后约 **54ms** 完成全部 join（2026-11-10）。

---

## 5. 新增线程检查清单

- [ ] 用 `thread_start()` 指定栈大小，并在本文档登记；
- [ ] 明确退出条件（标志 + `poll`/`cond` 超时），保证 `join` 能在 1s 内返回；
- [ ] 共享数据要么走锁，要么用 `__atomic`（单一标量）；
- [ ] 不在持锁状态下调用其它模块的加锁接口（除第 3 节的链）；
- [ ] 加入 `health_beat()`（若属于被监控模块）。
