# Edge-AI-Gateway 剩余工作清单（推进用）

> 用途：把“已完成 / 待验证 / 未开始”逐项列清，按优先级一步步推进。
> 每完成一项，把状态改为 ✅ 并在文末“推进记录”追加一行（日期 / 项目 / 结果）。
>
> 相关文档：
> - `docs/verification-guide.md`：每个阶段的验证命令与验收记录
> - `docs/v2-improvement-plan.md`：T1–T14 原始方案
> - `cloud/README.md`：云端服务（精判 + OTA + MQTT）用法

---

## 0. 当前基线（2026-11-11）

| 项 | 值 |
|---|---|
| 板端版本 | `2.8.0`（`current -> releases/2.8.0`，`state=CONFIRMED`） |
| 内核 | 4.1.15 #19，IIO buffer/trigger 已启用 |
| 设备 ID | `gw-b8ae1d010000` |
| MQTT | `192.168.3.26:1883`，账号 `gateway`（密码见 `cloud/secrets/`），配置文件 `gateway.json` |
| OTA | `http://192.168.3.26:8000/ota/manifest.json`，HMAC-SHA256 强制校验，密钥 `/opt/edge-gateway/ota/ota.key` |
| 云端 | `cloud/cloudctl.sh`（8000 精判+OTA，1883 MQTT，chrony 授时） |
| 时间 | SNTP 同步 UTC，应用默认 `TZ=CST-8`；shell 需 `echo CST-8 > /etc/TZ` |
| 开发机 | VMware Ubuntu `192.168.3.26`，NFS rootfs + TFTP + 云端服务 |

常用命令：

```bash
# 板端
killall lvgl_gateway
GATEWAY_LOG_LEVEL=debug /opt/edge-gateway/current/lvgl_gateway &
tail -f /opt/edge-gateway/logs/gateway.log

# 开发机
cd ~/shared/XRY/Edge-AI-Gateway
make deploy            # 构建 + 部署（先 killall）
cd cloud && ./cloudctl.sh start|status|restart|stop
./make_ota_pkg.sh <ver>   # 造签名 OTA 包
```

状态图例：✅ 完成并验证 ｜ 🟡 代码完成待验证 ｜ 🟠 部分完成 ｜ ❌ 未开始 ｜ ⏸ 暂缓

---

## 1. 总览

| # | 项目 | 优先级 | 状态 | 入口 |
|---|---|---|---|---|
| A1 | T5-A 事件总线 + 告警状态机验证 | P0 | 🟡 | 3.16 |
| A2 | T13 动态帧率验证 | P0 | ✅ | 3.20 |
| B1 | 线程栈大小 / 锁顺序规范化 | P1 | 🟡 | `docs/locking.md` |
| B2 | HTTP 接口鉴权（精判/OTA） | P1 | ✅ | 3.17 |
| B3 | 设备影子（desired/reported） | P2 | ✅ | 3.17 |
| B4 | 统一错误码表 | P2 | ✅ | `docs/error-codes.md` |
| C1 | 多源事件融合（摄像头+传感器时间窗） | P2 | 🟡 | 3.22 |
| C2 | 峰值检测（滑窗/RMS/双阈值/IMU 切 LIN） | P2 | 🟡 | 3.21 + 3.26 |
| C3 | IMU 姿态补偿 + 温漂自动拟合 | P3 | 🟡 | 3.23 + 3.27 |
| C4 | 摄像头 ROI/阴影抑制 | P3 | 🟡 | 3.24 |
| C5 | 多传感器置信度评分/规则引擎 | P3 | 🟡 | 3.22 |
| UI | 单摄 + 四防区状态卡（固定归属 camera_zone） | P3 | ✅ | 本文 UI + 3.30 |
| D1 | PC 端单元测试（config/event_store/sensor_mgr/fusion/ota_hmac） | P1 | ✅ | 3.25 |
| D2 | 24h 稳定性测试报告 | P1 | ❌ | 本文 D2 |
| D3 | 性能数据回填（CPU/RAM/FPS/延迟/重连/OTA） | P1 | 🟠 | `docs/performance.md` |
| D4 | 故障注入报告汇总 | P2 | 🟠 | `docs/fault-injection.md` |
| E1 | TLS（单向，已验证）/ mTLS（未做） | P2 | ✅ | 3.28 |
| E2 | 命令 ACK + 超时重试（已验证） | P3 | ✅ | 3.29 |
| E3 | 功耗（governor/背光/实测电流） | P3 | 🟠 | 本文 E3 |
| E4 | 系统级 OTA（A/B 分区） | P4 | ⏸ | 本文 E4 |
| E5 | T14 口径确认（测试报告 vs 系统 OTA） | P0 | ✅ | 本文 E5 |

---

## 2. 待验证项（先做，成本最低）

### A1. 事件总线 + 告警状态机（T5-A）

- **现状**：✅ 主链路已验证——运动触发 → `alarm zone=0 code=1 src=STRANR`；
  云端复核判定无人员 → `code=0 src=云端核：判定无人员` 自动消警。
- **已实测（2026-11-11）**：`event_bus` 分发、`alarm_fsm` TRIGGER→ACTIVE→CLEAR、云端纠偏。
- **剩余**：人工消警（长按 → MUTED）、超时恢复（RECOVERY）两项交互验证。
- **步骤**：
  1. 布防 → 人走过 → 告警；
  2. 告警弹窗长按消警 → 观察状态 `MUTED` → 回 `NORMAL`；
  3. 触发后不处理，等待超时 → 观察 `RECOVERY`。
- **验收标准**：时间轴/声光/状态机三者一致；无重复刷屏。

### A2. 动态帧率（T13）

- **现状**：✅ 已验证——idle 5fps / active 30fps / hold 10s 全部按预期切换。
- **实测（2026-11-11）**：`fps mode -> active(30fps)` 与 `idle(5fps)` 多轮循环，间隔约 10s。
- **步骤**：

```bash
killall lvgl_gateway
GATEWAY_LOG_LEVEL=debug /opt/edge-gateway/current/lvgl_gateway &
sleep 10
grep -E "dynamic fps|fps mode|conv=" /opt/edge-gateway/logs/gateway.log | tail -6
top -b -n 1 | head -6
# 人走过画面后再查一次
```

- **验收标准**：idle `conv=5 fps`、CPU 30~40%；走动后 `fps mode -> active(30fps)`、`conv=30 fps`；停止 10s 回 idle。
- **记录到**：3.20 验收表。

---

## 3. 工程完善

### B1. 线程栈大小 / 锁顺序

- **现状**：✅ 代码完成——`thread_util.c` 统一创建线程并显式指定栈大小；
  锁顺序、线程清单、退出顺序已整理到 `docs/locking.md`。
- **待验证**：板端启动/退出/长稳观察（栈大小调整后需确认无栈溢出）。
- **步骤**：
  1. ~~为线程创建加 `pthread_attr_setstacksize`~~（已由 `thread_start()` 完成）；
  2. ~~梳理锁顺序并成文~~（`docs/locking.md` 第 3 节）；
  3. 板端：`killall` 退出 <1s、无 crash、`dmesg` 无异常。
- **验收**：`top` 内存不升高；退出仍 <1s；`docs/locking.md` 与实际一致。

### B2. HTTP 接口鉴权（精判 / OTA）

- **现状**：✅ 代码完成并验证——`server.py` 增加 `X-API-Key` 中间件（`/health` 开放）；
  板端 `cloud_detect.c` / `http_get.c` 带 Key；Key 存 `gateway.json` 的 `cloud_api_key`。
- **实测（2026-11-11）**：无 Key 401、带 Key 200；OTA 检查 `up to date` 无 401。
- **步骤**（已完成）：
  1. `cloud/gen_api_key.sh` 生成 `api.key`，`cloudctl.sh restart`；
  2. 板端 `set_config` 下发 `cloud_api_key`；
  3. 重启应用，精判与 OTA 正常；
- **验收**：401 与正常路径都符合预期；板端日志无 401 错误。

### B3. 设备影子（desired/reported）

- **现状**：✅ 代码完成并验证——连接后与配置变更时发布
  `gateway/{id}/shadow/reported`（配置快照，不含密码/Key）；
  用 `config_generation()` 判断变化，不轮询字段。
- **实测（2026-11-11）**：连接 `gen=0`（version 2.1.0）；`set_config brightness=70` → `gen=1`；
  恢复 80 → `gen=2`；影子中无 `mqtt_pass`/`cloud_api_key`。
- **步骤**（已完成）：
  1. 板端升级到含本改动的版本；
  2. VM 订阅 `gateway/#`；
  3. 重启应用 → 收到 reported；`set_config` 改一项 → 再收到 reported。
- **验收**：reported 字段与 `gateway.json` 一致；无密码/Key 泄露；断网重连后补发。

### B4. 统一错误码

- **现状**：✅ 代码完成并验证——`error_code.h/.c` 定义模块号 + 错误号与 `err_str()`；
  OTA（manifest/fetch/sha/hmac/extract/install）与 HTTP（connect/status）已接入。
- **实测（2026-11-11）**：坏 hmac 包日志输出
  `hmac mismatch [OTA:HMAC]` / `hmac check failed, reject 2.3.0 [OTA:HMAC]`。
- **说明**：老模块的枚举（`sensor_status_t`、`ota_state_t`）保留，按需逐步接入。
- **验收**：`grep "\[OTA:" gateway.log` 能检索到错误码；文档 `docs/error-codes.md` 与代码一致。

---

## 4. 算法增强（按需）

### C1. 多源事件融合

- **现状**：🟡 代码完成——`fusion.c` 按分区缓存 8 条事件，3s 时间窗内摄像头+传感器
  共现才升级告警；单源封顶 60，低于阈值只做轻提醒。
- **开关**：`fusion_on`（默认 0，保持原语义）。
- **待验证**：单源只记事件、双源正常告警、关闭后恢复即时告警。

### C5. 置信度评分 / 规则引擎

- **现状**：🟡 与 C1 同一模块——基础分 55、双源 +25、阈值默认 70；
  后续可把 C2 的 `RMS/峰值` 作为加权项（已保留接口位置）。

### C2. 峰值检测（滑动窗口/双阈值/RMS）

- **现状**：🟡 代码完成——32 点滑窗统计 **`lin`（去重力线性加速度）**，静止 `rms/peak≈0`；
  同时完成：
  1. **双阈值**：每分区 `threshold_warn`（预警）/ `threshold`（告警），
     中间区间只做 toast + 时间轴 low，不触发告警与声光；设置页新增 `PRE` 滑条（0=OFF）；
  2. **IMU 判定切到 LIN**：`IMU_DATA_MAG` 的判定值改为去重力 `lin`，阈值 30/15，
     旧配置（v1，150）启动时自动迁移；
  3. buffer 模式预警不生效的问题已修（判定移到公共块）。
- **待验证**：轻晃（15~30）只预警、明显晃动（>30）告警、静止停止。
- **验收**：`[BRIDGE] zone=3 pre-warn`；时间轴“仓库 传感器预警”；蜂鸣器不响。
- **后续**：规则引擎（C5）可把预测/告警两档组合成更细的分级。

### C3. IMU 姿态补偿 / 温漂拟合

- **现状**：🟡 代码完成——姿态补偿（重力分离）已验证静态基线（`mag=52` 时 `lin=0`）；
  `rms/peak` 统计 `lin`；**温漂自动拟合**已实现：
  - 每次零偏采集记录一个 `(温度, 三轴均值)` 点（最多 4 个，同温度覆盖）；
  - ≥2 点且温差 ≥2℃ → 最小二乘拟合每轴斜率 `k`（LSB/℃×1000）并持久化；
  - 补偿公式 `ax -= (T - T_ref) * k / 1000`；`k=0` 时不补偿；
  - 已有斜率但新点温差不足时，基线折算回参考温度，避免重复补偿。
- **待验证**：晃动/换角度 `lin` 上升与回零；两次不同温度校准后
  `temp drift fitted` 且重启后 `k` 保留。
- **验收**：`[CALIB] temp drift fitted: k=[..] ref=..`；`lin` 在不同温度下静止仍≈0。

### C4. 摄像头 ROI / 阴影抑制

- **现状**：🟡 代码完成——`GATEWAY_CAM_ROI` 百分比矩形 + `GATEWAY_CAM_SHADOW=1`
  轻微变暗抑制；默认关闭。
- **待验证**：ROI 外不触发、ROI 内触发；阴影变化不持续命中。
- **验收**：启动日志 `motion roi=on [...] shadow_suppress=1`；行为符合预期。

---

### UI. 单摄 + 四防区状态卡

- **现状**：✅ 摄像头为单路固定安装（`/dev/video0`），**不做 4 通道切换**；
  UI 为「一路预览 + 四张防区状态卡」，四张卡显示传感器/布防/告警状态。
- **告警归属**：运动与精判结论统一归属 `config.camera_zone`（默认 0=前门），
  不再按“当前通道”归属（固定镜头切标签没有物理意义）。
- **本次清理**：删除 `cam_to_zone`、`g_current_cam`、4 个切换 pill；
  抓拍/时间轴改用防区名。
- **验证**：`set_config {"camera_zone":3}` 后 UI 文案与摄像头告警归属均为“仓库”（已通过）。
- **镜像**：默认 `V=1,H=0` 已确认方向正常；`GATEWAY_CAM_VFLIP/HFLIP` 可覆盖。

## 5. 质量与报告

### D1. PC 端单元测试

- **现状**：✅ 已完成并跑通——`tests/` + `make test`，6 个模块，输出 `ALL TESTS PASSED`。
- **覆盖**：`ota_hmac` / `config_mgr` / `event_store` / `sensor_mgr` / `fusion` / `sensor_calib`。
- **不在范围**：`cam_feed`（V4L2/LVGL）、`detector`（ncnn）、MQTT/OTA 网络路径。
- **已知噪声**：`-Wformat-truncation` 告警来自主机 gcc（路径拼接），非错误。

### D2. 24h 稳定性测试

- **做法**：板端常驻 24h，定时采集：`top`、`free`、`conv fps`、`MQTT pending`、`dmesg`；每小时记录一条。
- **验收**：无重启、无 STALE 持续、内存无增长趋势、断网自愈、OTA 后仍可确认。
- **输出**：`docs/stability-24h.md`（表格 + 结论）。

### D3. 性能数据回填

- **现状**：🟠 已建 `docs/performance.md`，把已有实测数据入表；测量缺口明确标注并附采集命令。

| 指标 | 采集方式 | 目标 | 当前 |
|---|---|---|---|
| CPU（active） | `top -b -n1` | ≤65% | ✅ ≈56%（conv=15fps） |
| CPU（idle） | `top -b -n1` | ≤40% | ⏳ 待测 |
| RAM | `ps`/`VmRSS` | 稳定，无增长 | 🟡 VSZ 114MB；RSS/趋势待测 |
| FPS（sensor/conv） | CAM 日志 | sensor 30，conv 5/30 | ✅ |
| 检测延迟 | 日志时间差 | 本地 ≤1s，云端 ≤2.5s | ⏳ 待测 |
| 重连时间 | MQTT 日志 | ≤60s | ✅ |
| OTA 耗时 | 日志时间差 | 下载+安装 ≤10s（局域网） | ⏳ 待测 |
| 优雅退出 | 日志时间差 | <1s | ✅ 54ms |

→ 详细数据与采集命令见 `docs/performance.md`；缺口建议与 D2 挂机一起采。

### D4. 故障注入报告

- **现状**：🟠 已生成 `docs/fault-injection.md`（12 类场景：传感器掉线/IMU unbind/KEY0 阻塞/
  断网补传/MQTT 认证/HTTP 鉴权/OTA 篡改/坏版本回滚/看门狗/单实例/光照突变/静态对照）。
- **待确认**：报告内容需你过目；2 项如实标为待验证（sha256 篡改未单独构造、C4 ROI/阴影）。
- **输出**：`docs/fault-injection.md`（场景 / 命令 / 预期 / 实测 / 结论）。

---

## 6. 大项（视时间）

### E1. TLS / mTLS

- **现状**：✅ **单向 TLS 已验证**（板端 `TLS handshake ok` → 8883 连接成功），
  `mqtt_transport.c` + mbedTLS 2.28 + 自签 CA；客户端仍用用户名/密码。
  mTLS（客户端证书）**未做**。
- **前置**：`scripts/build_mbedtls.sh` 交叉编译、`cloud/gen_tls_certs.sh` 生成证书；
  时间同步已完成（证书有效期校验依赖）。
- **待验证**：板端握手成功、错误 CA 被拒、`mqtt_tls=0` 回退 1883 正常。
- **验收**：见 `verification-guide` 3.28。
- **mTLS（可选 +0.5 天）**：生成客户端证书 → `mbedtls_ssl_conf_own_cert` → broker
  `require_certificate true`。

### E2. 命令 ACK + 超时重试（精简版）

- **现状**：✅ 精简版已完成并验证：
  - 设备端：命令 id 去重（16 条），重试只补 ACK、不重复执行（保护 OTA）；
  - 云端：`cmd_send.py` + `POST /cmd`，超时重发（默认 2s×3）。
- **已舍弃**：设备注册、心跳/能力上报、云端设备表、区域联防去重。
- **实测**：正常 `attempts=1`；断线 `attempts=4, ok=false`；同 id 第二次
  `duplicate -> ack only`。
- **验收**：见 `verification-guide` 3.29。

### E3. 功耗

- governor 策略（当前手动 `performance`）；
- 背光自动 + 夜间模式；
- 实测电流：待机/预览/告警/OTA 四态。
- **验收**：待机电流下降可量化；动态帧率与 governor 联动。

### E4. 系统级 OTA（⏸）

- A/B 分区、U-Boot 启动计数、回滚；需改分区表，风险高。
- **验收**：升级失败自动回退到旧分区并正常启动。

### E5. T14 口径确认（✅ 已定稿）

- **结论**：**T14 = 测试与报告**（单测/集成/故障注入/性能/稳定性 + 文档）；
- 系统级 OTA（A/B 分区、U-Boot 启动计数、回滚）不属于 T14，单列为 **E4（⏸ 暂不做）**；
- 已同步 `v2-improvement-plan.md` 第 7.4 节与总览表。

---

## 7. 建议推进顺序

| 批次 | 内容 | 预计 | 依赖 |
|---|---|---|---|
| 第 1 批 | A1、A2（先补验证） | 0.5 天 | 板子在手 |
| 第 2 批 | D1 单元测试 | 1~2 天 | 无 |
| 第 3 批 | D2 + D3 + D4（长稳/性能/故障报告） | 1 天（含 24h 挂机） | 板子常开 |
| 第 4 批 | B1 + B4 | 1 天 | 无 |
| 第 5 批 | B2 + B3 | 1~2 天 | 云端 |
| 第 6 批 | C1 + C2 | 2 天 | 板子 |
| 第 7 批 | E1（mbedTLS）或 E2/E3 | 3~5 天 | 视目标 |

---

## 8. 推进记录

| 日期 | 项目 | 结果 | 备注 |
|---|---|---|---|
| 2026-11-11 | 建立本文档 | — | 基线 1.9.0 |
| 2026-11-11 | D1 PC 单元测试 | ✅ | `make test` → `ALL TESTS PASSED`（5 模块） |
| 2026-11-11 | C2 双阈值 + LIN 语义 | 🟡 | 代码完成，待板端验证；旧配置自动迁移 |
| 2026-11-11 | C3 温漂自动拟合 | 🟡 | 6 模块单测通过；待板端双温度验证 |
| 2026-11-11 | 单元测试全量 | ✅ | `ALL TESTS PASSED`（6 模块） |
| 2026-11-11 | 新符号校验 | ✅ | 二进制含 `pre-warn` / `temp drift fitted` |
| 2026-11-11 | E5 T14 口径 | ✅ | T14=测试与报告；系统级 OTA 单列 E4 |
| 2026-11-11 | T11 单向 TLS | ✅ | 板端 `TLS handshake ok`，8883 连接成功 |
| 2026-11-11 | T12 精简版 | ✅ | ACK 重试 + id 去重；板端三条用例通过 |
| 2026-11-11 | UI 摄像头-分区联动 | ✅ | 已生效，清理过期占位 |
| 2026-11-11 | 单摄语义 + 镜像 | ✅ | 去掉 4 通道切换；`camera_zone` 归属；`flip v=1 h=0` |
| 2026-11-11 | D3 性能汇总 | 🟠 | `docs/performance.md` 已建成；CPU idle/RSS/延迟/OTA 耗时待测 |
| 2026-11-11 | D4 故障注入报告 | 🟠 | `docs/fault-injection.md` 已出，待审阅 |
