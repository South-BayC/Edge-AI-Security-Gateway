# 性能数据汇总（D3）

> 硬件：i.MX6ULL（单核 Cortex-A7 @528MHz）+ OV5640 + AP3216C + ICM20608
> 软件：Linux 4.1.15 #19，Buildroot rootfs（NFS），LVGL 9.2.2，应用版本 2.7.0
> 采集时间：2026-11-10 ~ 2026-11-11
> 说明：本表只收录**板端实测**数据；未采集的项标「待测」，并在第 6 节给出采集命令。

---

## 1. 汇总

| 指标 | 目标 | 实测 | 状态 |
|---|---|---|---|
| CPU（active，conv=15fps） | ≤65% | ≈56% | ✅ |
| CPU（idle，conv=5fps） | ≤40% | 未记录精确值 | ⏳ 待测 |
| 内存 VSZ | 稳定 | 114 MB（与改造前一致） | ✅ |
| 内存 RSS / 增长趋势 | 无增长 | 未采集 | ⏳ 待测 |
| 预览帧率（active） | 30fps | `sensor=30~31`，`conv=30` | ✅ |
| 预览帧率（idle） | 5fps | `conv=5` | ✅ |
| 单帧转换耗时 | — | `conv_ms≈24.3`（15fps 时期） | ✅ |
| 检测延迟（本地） | ≤1s | 未记录端到端数值 | ⏳ 待测 |
| 云端精判往返 | ≤2.5s | 未记录端到端数值 | ⏳ 待测 |
| MQTT 命令响应 | ≤1s | ≈200ms | ✅ |
| MQTT 重连 | ≤60s | 指数退避 1s→60s，队列恢复 ≤60s | ✅ |
| 优雅退出 | <1s | 54ms | ✅ |
| OTA 下载+安装 | ≤10s（局域网） | 未记录耗时 | ⏳ 待测 |

---

## 2. CPU

### 2.1 已测：定点帧率时期

`conv=15 fps`（固定 50ms 间隔）下：

```
VSZ 114m，CPU ≈56%
```

结论：与 T5 线程模型改造前持平，说明锁/队列/精判线程化没有引入额外 CPU 开销。

### 2.2 已测：动态帧率行为

```
[CAM] dynamic fps: idle=200ms(5fps) active=33ms(30fps) hold=10000ms
[CAM] fps mode -> idle(5fps)      sensor=30 fps, conv=5  fps
[CAM] fps mode -> active(30fps)   sensor=30 fps, conv=30 fps
```

多轮切换（间隔约 10s 回落）均按预期，未出现卡在某一档或抖动。

### 2.3 待测：idle/active 的 CPU 精确值

设计预期：idle 30~40%，active 回落 ~56%。需板端用 `top` 采两次（静止 / 有人走动）。

---

## 3. 内存

| 项 | 值 | 说明 |
|---|---|---|
| VSZ | 114 MB | 含 LVGL 画布、ncnn 模型（mmap）、线程栈 |
| 帧缓冲/画布 | 未单列 | 640×480 RGB 双缓冲 |
| 线程栈 | 显式指定 | 见 `docs/locking.md`，`thread_start()` 统一设置 |

RSS 与长时趋势归入 D2（24h 稳定性）一并采集。

---

## 4. 延迟

### 4.1 已测：MQTT 命令

| 场景 | 改造前 | 现在 |
|---|---|---|
| `set_config` / `set_warn` / `ota` 命令响应 | ≈30s（等 ping 周期才读包） | ≈200ms |

修复点：空闲等待 1s → 200ms，且等待期间也调用 `mqtt_read_incoming()`。

### 4.2 已测：本地精判异步化

```
[DET] worker started, queue depth=2
```

精判（ncnn）从采集线程拆到独立 worker + 有界队列（深度 2），采集线程不再被推理阻塞；
队列满时丢最旧，保证预览不掉帧。

### 4.3 待测：端到端

需要两个时间戳相减：

- 本地：`motion hit` 时间 → `alarm zone=.. code=1` 时间；
- 云端：HTTP 请求发出 → 判定响应（`[CLOUD]` 日志）。

---

## 5. 网络与 OTA

### 5.1 重连

| 行为 | 参数 | 实测 |
|---|---|---|
| 断连重试 | 1s 起指数退避，上限 60s | 恢复后队列在 ≤60s 内清空 |
| 空闲心跳 | 每 30s PINGREQ | 120s 观察无 `NET heartbeat timeout` |
| 断连时心跳 | 每 5s 回外层重连 | 无「既不心跳也不重连」的卡死 |
| 事件补传 | 落盘 `events.jsonl`，发送成功按 id 删除 | 断网 4 条不丢，重启后 `pending=2 → 0` |

### 5.2 OTA

| 项 | 值 |
|---|---|
| 包 | 外层 tar.gz（manifest + payload + checksums）+ HMAC-SHA256 |
| 校验顺序 | sha256（外层）→ hmac（manifest+payload）→ 解包 → 安装 |
| 生效 | 安装成功 `sync + reboot`，`PENDING_CONFIRM → CONFIRMED` |
| 回滚 | 连续 2 次启动未确认，第 3 次自动回滚 |
| 耗时 | ⏳ 待测（需记录 `check start` → `installed` 的时间差） |

### 5.3 日志

| 项 | 值 |
|---|---|
| 异步队列 | 64 行 × 384B |
| 单文件 | 256KB，保留 4 个历史文件 |
| 满队策略 | 丢弃并计数（`dropped`），不阻塞业务线程 |

---

## 6. 缺口与采集命令

### 6.1 CPU（idle / active）

```bash
# 【板端】先确保用 debug 启动，观察 conv 帧率
killall lvgl_gateway
GATEWAY_LOG_LEVEL=debug /opt/edge-gateway/current/lvgl_gateway &
sleep 15
grep -E "fps mode|sensor=.*conv=" /opt/edge-gateway/logs/gateway.log | tail -4

# 采 3 次，取稳定值（第 1 次含启动开销）
top -b -n 3 -d 2 | grep -E "^ *[0-9]+ .*lvgl_gateway|Cpu"
```

记录：idle（无人）时 `%CPU`；人走过画面后 10s 内再采一次得 active 值。

### 6.2 内存

```bash
# 【板端】RSS（KB）
ps -o pid,vsz,rss,comm -C lvgl_gateway
# 或
grep VmRSS /proc/$(pidof lvgl_gateway)/status
```

### 6.3 本地检测延迟

```bash
# 【板端】取最近一次命中与告警的时间戳
grep -E "motion hit|alarm zone" /opt/edge-gateway/logs/gateway.log | tail -4
```

两条日志的毫秒时间差即本地端到端延迟（含融合窗口 3s，若 `fusion_on=1` 需注明）。

### 6.4 云端精判往返

```bash
# 【板端】
grep -E "\[CLOUD\]|cloud detect" /opt/edge-gateway/logs/gateway.log | tail -4
# 【开发机 VM】server.py 访问日志可对照请求时间
```

### 6.5 OTA 耗时

```bash
# 【板端】MQTT 触发后
grep -E "OTA|update .* ready|installed" /opt/edge-gateway/logs/gateway.log | tail -8
```

记录 `check start`（或 `cmd check`）到 `installed` 的时间差，单位为秒。

---

## 7. 结论

- 定点帧率时期 CPU ≈56%，线程化改造未增加开销；
- 动态帧率（5/30fps）行为正确，idle 省 CPU 的**量化值**待补；
- 内存 VSZ 稳定在 114MB，RSS 与长时趋势由 D2 覆盖；
- MQTT 命令延迟从 30s 降到 200ms，重连上限 60s，事件不丢；
- 剩余缺口都是**测量项**（不影响功能），建议与 D2 24h 挂机一起采：CPU/RSS/内存趋势/OTA 耗时。
