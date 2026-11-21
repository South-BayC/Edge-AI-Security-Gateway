# Edge-AI-Gateway 阶段验证手册

> 版本：v1.0（2026-11-09）
> 用途：记录 v2 各阶段功能的**编译、运行、验收命令**，便于后续复测和面试演示。
> 约定：命令默认在 **Ubuntu VM** 的项目根执行；板端命令单独标注。
> 项目根（VM）：`~/shared/XRY/Edge-AI-Gateway`
> 项目根（Windows）：`C:\Users\SouthBay\Desktop\举目长安\嵌入式Linux\Edge-AI-Gateway`

---

## 0. 环境与编译

### 0.1 路径要求

项目绝对路径**不能包含空格**，否则 GNU Make 会把路径拆成多个目标（曾出现 “没有规则可制作目标 AI”）。

当前正确路径：

```bash
cd ~/shared/XRY/Edge-AI-Gateway
pwd
# 应为 /home/szh/shared/XRY/Edge-AI-Gateway，无空格
```

### 0.2 编译

```bash
# 全量：驱动 + 设备树 + LVGL 应用
make

# 只编 LVGL 应用
make lvgl

# 只编驱动 / 设备树
make core
make dts

# 清理（路径变更后建议执行一次）
make clean
```

产物：

| 产物 | 路径 |
|---|---|
| LVGL 应用 | `build/lvgl/lvgl_gateway` |
| 驱动 `.ko` | `build/module/*.ko` |
| 设备树 `.dtb` | `build/dts/*.dtb` |

### 0.3 部署

```bash
make deploy
```

一条命令完成“构建 + 部署”（等价于原 `make send` + 应用 release 同步）：

| 内容 | 目标位置 |
|---|---|
| 驱动模块 `build/module/*.ko` | `$(NFS_ROOT)/lib/modules/4.1.15/` |
| 设备树 `build/dts/*.dtb` | `$(TFTP_DIR)/` |
| 应用 `lvgl_gateway` | `$(NFS_ROOT)/usr/bin/`（出厂/兜底版本） |
| 应用 `lvgl_gateway` | `$(NFS_ROOT)/opt/edge-gateway/current/`（当前生效 release） |
| 模型 `face.param` / `face.bin` | `$(NFS_ROOT)/root/` |

路径可用变量覆盖：

```bash
make deploy NFS_ROOT=/path/to/rootfs TFTP_DIR=/path/to/tftp
```

如果 `current` 不存在或指向无效目录，会打印 `skip release copy: current 未指向有效目录`。

> `/opt/edge-gateway` 权限为 `0700`（配置/日志/事件含敏感信息），`make deploy`
> 内部用 `sudo` 解析 `current` 并复制，执行时会要求输入密码。

> 部署前务必先 `killall lvgl_gateway`，NFS 上直接覆盖正在执行的二进制
> 可能出现 `Text file busy` 或进程异常。
>
> 注意：Makefile 的 `.o` 规则只跟踪 `lv_conf.h`，**只改头文件（.h）时不会触发
> 依赖它的 .c 重编**，需要 `touch` 对应 .c 或删掉 `build/cache/lvgl/src/<模块>.o`。

### 0.4 板端开机自启（rcS）

`/etc/init.d/rcS` 负责三件事：

1. 按数字顺序执行 `/etc/init.d/S??*`，**必须传 `start` 参数**；
2. `depmod -a` 后 `modprobe` 各驱动模块；
3. 启动应用。

关键点（均已修正并验证，2026-11-10）：

- 原脚本写成 `"$i" starti`（拼写错误），所有 S 脚本只打印
  `Usage: /etc/init.d/Sxx {start|stop|restart|reload}` 就退出，
  `syslogd` / `klogd` / `mdev` / `crond` 实际都没启动；已改为 `"$i" start`；
- `depmod -a` 必须在 `modprobe` 循环**之前**，否则模块依赖表可能未就绪；
- 应用启动必须指向 `current`，否则 OTA 切换不生效：

```sh
if [ -x /opt/edge-gateway/current/lvgl_gateway ]; then /opt/edge-gateway/current/lvgl_gateway & else /usr/bin/lvgl_gateway & fi
```

验证：

```bash
ps | grep -E "syslogd|klogd|crond|mdev"
pidof lvgl_gateway
readlink /opt/edge-gateway/current
grep -E "application start|OTA" /opt/edge-gateway/logs/gateway.log | tail -3
```

预期：

- `syslogd` / `klogd` / `mdev` / `crond` 进程存在；
- 应用自动启动，日志有新的 `application start`；
- 开机串口不再出现 `Usage: /etc/init.d/...`。

已知无害提示：

```
Starting network: ip: RTNETLINK answers: File exists
FAIL
```

内核 cmdline（`ip=192.168.3.24:...`）已配置好 eth0，`ifup -a` 再次配置会失败，
不影响网络与 NFS。若想消除，可把 `/etc/network/interfaces` 中的 eth0 改为 `manual`。

回退：

```bash
cp /etc/init.d/rcS.bak /etc/init.d/rcS
reboot
```

验收记录：

| 日期 | 项目 | 结果 | 备注 |
|---|---|---|---|
| 2026-11-10 | `starti` → `start` 修复 | 通过 | 开机 `Starting syslogd/klogd/mdev/crond: OK` |
| 2026-11-10 | 应用开机自启 | 通过 | `pidof lvgl_gateway` 有值，指向 `current` |
| 2026-11-10 | depmod 顺序 | 通过 | `depmod` 在 `modprobe` 之前 |

---

### 0.5 时间同步（SNTP，2026-11-10）

板端无 RTC 电池，开机时间从 1970 开始（`snvs_rtc` 初始值），日志时间不可信，
后续 TLS 证书有效期校验也会失败。

实现：`time_sync.c` 内置最小 SNTP 客户端（UDP 123，无第三方依赖）：

- 启动后立即同步一次；失败每 5 分钟重试；
- 成功后每 6 小时再校准一次；
- 服务器：`GATEWAY_NTP_SERVER`，默认 `192.168.3.26`（开发机）。

#### 开发机做 NTP 服务器（VM）

```bash
sudo apt install -y chrony
echo "local stratum 10"     | sudo tee -a /etc/chrony/chrony.conf
echo "allow 192.168.3.0/24" | sudo tee -a /etc/chrony/chrony.conf
sudo systemctl restart chrony
chronyc tracking
```

> `local stratum 10` 让开发机即使没有上游时钟也能对局域网授时；
> 有外网时会自动同步公网时间后再转发。

#### 板端验证

```bash
killall lvgl_gateway
GATEWAY_NTP_SERVER=192.168.3.26 GATEWAY_LOG_LEVEL=debug \
  /opt/edge-gateway/current/lvgl_gateway &
sleep 3
date
grep -E "TIME" /opt/edge-gateway/logs/gateway.log | tail -3
```

预期：

```
[TIME] sntp started, server=192.168.3.26
[TIME] synced from 192.168.3.26 (1762...)
```

`date` 输出应为当前时间（不再是 1970）。之后的日志时间戳恢复正常。

> **时区**：系统时钟保持 UTC（SNTP 同步的就是 UTC）。应用启动时若未设置 `TZ`，
> 默认 `CST-8`（UTC+8），日志与 UI 显示为北京时间。
> Shell 里的 `date` 想显示本地时间，需要：
>
> ```bash
> echo CST-8 > /etc/TZ        # BusyBox 工具读取
> export TZ=CST-8             # 当前 shell
> date
> ```
>
> 若时间“日期对、时分不对且正好差 8 小时”，就是时区未设置，不是 SNTP 错误。

> 板端没有配置 DNS（内核 cmdline 未指定），所以 `ping ntp.aliyun.com` 会报
> `bad address`；用 IP 直连 NTP 即可，不需要 DNS。

验收记录：

| 日期 | 项目 | 结果 | 备注 |
|---|---|---|---|
| 2026-11-10 | SNTP 客户端 | 完成 | `time_sync.c`，启动同步 + 6h 校准 + 失败重试 |
| 2026-11-10 | 板端同步 | 通过 | `[TIME] synced from 192.168.3.26`，`date` 恢复正常时间 |

---

## 1. T1 配置持久化（已完成，2026-11-09）

### 1.1 功能范围

- 云端复核开关、帧差间隔、灰度差阈值；
- 背光亮度；
- 定时布防开关/起止时间；
- 四防区触发源/通道/阈值/比较方式/使能、防抖次数、冷却时间；
- JSON 原子写（tmp → fsync → rename → 目录 fsync）；
- 损坏配置回退默认并回写修复。

### 1.2 编译检查

```bash
make lvgl
ls -l build/lvgl/lvgl_gateway
```

预期：编译通过，无 `config_mgr` 相关错误。

### 1.3 启动与配置路径

板端：

```bash
/usr/bin/lvgl_gateway &
```

查看日志（串口或终端）：

```bash
# 应出现类似：
# [CONFIG] use path /opt/edge-gateway/config/gateway.json
# [CONFIG] loaded /opt/edge-gateway/config/gateway.json (version 1)
#   或
# [CONFIG] /opt/... not found, use defaults
```

确认实际路径：

```bash
cat /opt/edge-gateway/config/gateway.json
ls -l /opt/edge-gateway/config/gateway.json
# 权限应为 -rw-------（600）
```

如果 `/opt` 不可写，日志会显示回退路径：

```
/etc/edge-gateway/gateway.json
./gateway.json
```

### 1.4 修改设置后落盘

1. 在屏幕上修改亮度/云端复核/帧差/预警阈值；
2. 等待约 1 秒（UI 的配置 flush 定时器）；
3. 检查文件：

```bash
cat /opt/edge-gateway/config/gateway.json
```

预期：对应字段变化，例如亮度从 80 变为新值。

### 1.5 重启保持

```bash
killall lvgl_gateway
/usr/bin/lvgl_gateway &
```

预期：设置弹窗中的值、预警阈值、定时布防状态与重启前一致。

### 1.6 损坏配置回退

```bash
# 备份后写入非法 JSON
cp /opt/edge-gateway/config/gateway.json /tmp/gateway.bak
echo '{bad json' > /opt/edge-gateway/config/gateway.json

killall lvgl_gateway
/usr/bin/lvgl_gateway &
```

预期：

- 日志出现 `[CONFIG] JSON parse failed, use defaults`；
- 应用正常启动，使用默认配置；
- 1 秒后配置文件被修复为合法 JSON。

恢复：

```bash
cp /tmp/gateway.bak /opt/edge-gateway/config/gateway.json
```

### 1.7 验收记录

| 日期 | 项目 | 结果 | 备注 |
|---|---|---|---|
| 2026-11-09 | T1 编译 | 通过 | `make lvgl`，产物 3.5M |
| 2026-11-09 | T1 运行 | 通过 | 用户确认 |

---

## 2. T2 日志系统（已完成，2026-11-09）

### 2.1 功能范围

- 分级：DEBUG / INFO / WARN / ERROR / FATAL；
- 异步：业务线程写内存队列（64 行 × 384B），独立线程落盘；
- 轮转：单文件 256KB，保留 4 个历史文件；
- 控制台：同时输出到 stderr（串口调试）；
- 覆盖模块：`MAIN`、`CONFIG`、`BRIDGE`、`CAM`、`DET`、`LVGL`、`STORAGE`、`UI`、`SIM`。

### 2.2 编译检查

```bash
make clean
make lvgl
```

预期：`log_svc.c` 参与编译，无 `undefined reference to log_write` / `log_init` 等链接错误。

快速检查符号：

```bash
arm-linux-gnueabihf-nm build/lvgl/lvgl_gateway | grep -E "log_init|log_write|log_close"
```

### 2.3 启动日志

板端：

```bash
/usr/bin/lvgl_gateway &
```

预期串口出现：

```
[LOG] file=/opt/edge-gateway/logs/gateway.log level=INFO max=256KB x4
[2026-11-09 ...] [INFO] [MAIN] (tid=...) application start
[2026-11-09 ...] [INFO] [CONFIG] (tid=...) use path ...
[2026-11-09 ...] [INFO] [LVGL] (tid=...) fbdev /dev/fb0 ready (1024x600)
[2026-11-09 ...] [INFO] [BRIDGE] (tid=...) device bridge started
```

检查文件：

```bash
ls -l /opt/edge-gateway/logs/
# 预期：-rw------- （600，仅 root 可读写）
tail -50 /opt/edge-gateway/logs/gateway.log
```

### 2.4 级别过滤

```bash
# 只输出 WARN 及以上
GATEWAY_LOG_LEVEL=warn /usr/bin/lvgl_gateway &

# 输出 DEBUG（含人脸耗时）
GATEWAY_LOG_LEVEL=debug /usr/bin/lvgl_gateway &
```

预期：

- `warn` 级别下不出现 `[INFO]`/`[DEBUG]`；
- `debug` 级别下出现 `[DEBUG] [DET] faces=... (.. ms)` 和 `[DEBUG] [CAM] sensor=... fps`。

### 2.5 轮转

```bash
# 持续写入直到超过 256KB（可触发多次告警/人脸检测，或临时把 max 调小调试）
ls -l /opt/edge-gateway/logs/
```

预期出现：

```
gateway.log
gateway.log.1
gateway.log.2
gateway.log.3
```

最多保留 4 个文件；最旧的 `.3` 会被删除。

### 2.6 异步不阻塞

主观验证：

1. 打开 DEBUG 级别，持续产生日志；
2. 触摸界面、切换分区、打开设置，确认无明显卡顿；
3. `top` 观察 `lvgl_gateway` CPU 占用无异常尖峰。

### 2.7 退出与 flush

```bash
killall -TERM lvgl_gateway
tail -5 /opt/edge-gateway/logs/gateway.log
```

预期：

```
[INFO] [MAIN] application exit
```

终端出现：

```
[LOG] closed (dropped=0)
```

`dropped` 表示队列满丢弃的条数；正常应为 0，高频压测时可大于 0。

### 2.8 目录回退（可选）

```bash
# 指定一个不可创建的目录，观察是否自动回退到其他候选
GATEWAY_LOG_DIR=/proc/nonexistent /usr/bin/lvgl_gateway &
```

预期：

- `/proc/nonexistent` 不可创建时，自动尝试 `/opt/edge-gateway/logs`、`/var/log/edge-gateway`、`./logs`；
- 只有**所有候选都不可写**时，才输出 `[LOG] no writable dir, fallback to stderr`，日志只走 stderr，应用继续运行。

### 2.9 板端验证注意事项

1. **系统时间**：板端 RTC/NTP 未同步时，日志时间戳会显示 `1970-01-01`。排查前先确认：
   ```bash
   date
   # 若时间错误：先同步 RTC/NTP，或临时设置
   # date -s "2026-11-09 12:00:00"
   ```
2. **单实例**：启动前先停止旧进程，避免两个应用同时写同一个日志文件：
   ```bash
   killall lvgl_gateway 2>/dev/null
   /usr/bin/lvgl_gateway &
   ```
3. **驱动未加载的 WARN**：日志中的 `/dev/led open failed`、`imx6ull-gpio-keys not found`、
   `IIO ap3216c/icm20608 not found`、`S_FMT failed` 属于验证环境未 `insmod` 对应驱动，
   不是日志模块缺陷。做 T3 传感器验证前需先加载：
   ```bash
   insmod /lib/modules/4.1.15/led_drv.ko
   insmod /lib/modules/4.1.15/key_input.ko
   insmod /lib/modules/4.1.15/ap3216c.ko
   insmod /lib/modules/4.1.15/icm20608.ko
   ```
4. **日志权限**：`log_svc` 已把日志文件收紧为 `600`；若看到 `644`，说明运行的是旧版本，重新编译部署。

### 2.10 验收记录

| 日期 | 项目 | 结果 | 备注 |
|---|---|---|---|
| 2026-11-09 | T2 编译 | 通过 | `make lvgl`，`storage_mgr` 补 `<errno.h>` 后通过 |
| 2026-11-09 | T2 运行 | 通过 | 日志文件生成、INFO/DEBUG 切换、退出 flush（dropped=0） |

---

## 3. T3-A 传感器数据质量（已完成，2026-11-09）

### 3.1 功能范围

- AP3216C（ALS/PS/IR）与 ICM20608（AX/AY/AZ/TEMP）独立通道处理；
- 范围检查、变化率检查、3σ 离群剔除；
- EMA + 中值滤波；
- 连续 3 次异常/读取失败升级为故障；
- 数据陈旧（STALE）检测；
- 故障/恢复事件写入时间轴和日志；
- IMU 采样从 200ms 提升到 100ms（10Hz）。

### 3.2 编译检查

```bash
cd ~/shared/XRY/Edge-AI-Gateway
make lvgl
```

预期：`sensor_mgr.c` 参与编译，无 `undefined reference to sensor_mgr_*`。

### 3.3 驱动准备

T3-A 依赖 IIO 驱动，先加载：

```bash
insmod /lib/modules/4.1.15/ap3216c.ko
insmod /lib/modules/4.1.15/icm20608.ko

# 确认 IIO 设备
ls /sys/bus/iio/devices/
```

### 3.4 启动检查

```bash
killall lvgl_gateway 2>/dev/null
GATEWAY_LOG_LEVEL=info /usr/bin/lvgl_gateway &
tail -30 /opt/edge-gateway/logs/gateway.log
```

预期：

- 不再出现 `IIO ap3216c not found` / `IIO icm20608 not found`；
- 打开设置→测试面板，AP3216C/ICM20608 数值持续刷新；
- IMU 刷新明显比之前快（10Hz）。

### 3.5 故障注入：通信失败

```bash
# 卸载驱动 → 采集线程连续读取失败
rmmod icm20608
sleep 1
tail -20 /opt/edge-gateway/logs/gateway.log
```

预期：

```
[WARN] [SENSOR] ICM20608 fault: COMM_ERR
```

UI 时间轴出现 `ICM20608 异常`。

> 连续 3 次读取失败后进入 `COMM_ERR`（IMU 约 300ms、AP3216C 约 1.5s）；
> 随后关闭失效 fd，由 1s 重连块重新探测。若只看到 `STALE`，说明旧版本仍在运行。

恢复：

```bash
insmod /lib/modules/4.1.15/icm20608.ko
sleep 1
tail -20 /opt/edge-gateway/logs/gateway.log
```

预期（最多约 1 秒重连窗口）：

```
[INFO] [BRIDGE] ICM20608 reconnected: /sys/bus/iio/devices/iio:deviceX
[INFO] [SENSOR] ICM20608 recovered
```

UI 时间轴出现 `ICM20608 正常`。

> `dev_bridge` 每 1 秒重新探测一次 IIO 设备；`rmmod` 后旧 fd 失效时，
> 采集线程会关闭失效句柄并重新 `open` 新节点，因此恢复事件可能比 `insmod` 晚 1 秒左右。

AP3216C 同理：`rmmod ap3216c` / `insmod ap3216c.ko`。

#### ICM20608 在 buffer 模式下的故障注入

`icm20608` 挂 SPI（ecspi3），且 buffer 的 chardev 持有模块引用，直接 `rmmod` 会报
`Resource temporarily unavailable`。改用 spi 驱动的 `unbind` / `bind`：

```bash
# 卸载（模拟设备消失）
echo spi2.0 > /sys/bus/spi/drivers/icm20608/unbind; sleep 2
grep -E "IMU|SENSOR" /opt/edge-gateway/logs/gateway.log | tail -3

# 恢复
echo spi2.0 > /sys/bus/spi/drivers/icm20608/bind; sleep 4
grep -E "IMU|SENSOR" /opt/edge-gateway/logs/gateway.log | tail -3
```

实测（2026-11-10）：

```
[ERROR] [IMU] buffer read -1 bytes: No such file or directory, retry
[WARN]  [SENSOR] ICM20608 fault: COMM_ERR
[INFO]  [IMU] buffer enabled: /dev/iio:device1 (100Hz)
[INFO]  [SENSOR] ICM20608 recovered
[INFO]  [BRIDGE] ICM20608 reconnected: /sys/bus/iio/devices/iio:device1
```

> buffer 读失败后 `g_imu_buffer_ok=0`，采集线程的 sysfs 回退路径接手，连续 3 次读失败
> （约 300ms）升级为 `COMM_ERR`。恢复时 buffer 线程先重开设备，采集线程下一拍（1s）
> 才重新探测到节点，因此 `reconnected` 可能晚于 `recovered` 约 0.3 秒，属正常顺序。

### 3.6 数据陈旧（STALE）

如果采集线程被阻塞或设备不返回新数据，`sensor_mgr_check_stale()` 会产生 STALE。
可用 `kill -STOP` 暂停采集线程复现（需要先找到线程号，或直接观察拔出设备后的日志）：

```bash
# 可选：暂停整个进程，超过 stale 时间后恢复
kill -STOP $(pidof lvgl_gateway)
sleep 3
kill -CONT $(pidof lvgl_gateway)
tail -20 /opt/edge-gateway/logs/gateway.log
```

预期出现 `STALE` 故障，恢复后出现 `OK`。

### 3.7 滤波效果（主观）

- 静止时测试面板的 IMU 数值抖动应比原始值小；
- 快速遮挡/晃动时，单次毛刺不应立即触发告警；
- 持续超阈值仍能正常触发预警（防抖 + 冷却生效）。

### 3.8 T3-A 验收记录

| 日期 | 项目 | 结果 | 备注 |
|---|---|---|---|
| 2026-11-09 | T3-A 编译 | 通过 | `sensor_mgr.c` 参与编译 |
| 2026-11-09 | T3-A 故障 | 通过 | `rmmod` → `COMM_ERR` |
| 2026-11-09 | T3-A 恢复 | 通过 | `modprobe` → `reconnected` + `recovered` |
| 2026-11-10 | IMU buffer 故障 | 通过 | `unbind` → `buffer read -1` + `COMM_ERR` |
| 2026-11-10 | IMU buffer 恢复 | 通过 | `bind` → `buffer enabled` + `recovered` + `reconnected` |
| 2026-11-10 | `invalid_cnt` 清零回归 | 通过 | 故障→恢复后无“单次异常即升级”抖动 |
| 2026-11-10 | IMU stale 阈值调整 | 已调整 | 800ms → 1500ms，避免单核瞬时抢占误报（长稳观察中） |

### 3.9 T3-C 校准（已完成：静态基线，2026-11-09）

功能范围：

- 加速度零偏/比例校准（`ax' = (ax - off) * scale / 1000`）；
- 温度原始值偏置；
- 参数持久化到 `gateway.json` 的 `calib` 节；
- 上电静止零偏采集（`GATEWAY_CALIB_ONCE=1`）。

编译：

```bash
make lvgl
```

#### 上电零偏采集

```bash
killall lvgl_gateway 2>/dev/null
GATEWAY_CALIB_ONCE=1 /usr/bin/lvgl_gateway &
# 保持设备静止约 2 秒
tail -20 /opt/edge-gateway/logs/gateway.log
```

预期：

```
[INFO] [CALIB] zero bias start: keep device still for 2000 ms
[INFO] [CALIB] static baseline done: off=[x,y,z] temp_avg=... samples=...
```

UI 时间轴出现 `ICM20608 CAL OK`。

检查配置：

```bash
grep -A2 '"calib"' /opt/edge-gateway/config/gateway.json
```

预期 `"valid": 1`，`imu_off` 为测得的静态基线，`temp_off` 为 `0`。

> **重要语义**：`imu_off` 是“安装姿态静态基线”，包含重力分量（例如设备倾斜时
> `ay/az` 会有几百到 1700 的偏移），不是纯传感器零偏。
> - 固定安装场景：基线去除后，合加速度静态接近 0，振动阈值直接反映动态加速度；
> - 需要保留 1g 重力/做真正零偏时，使用下面的六面校准；
> - 当前版本不做温度校准，`temp_off` 固定为 0（重新采集会把旧值归零）。

#### 校准前后对比

- 测试面板 AX/AY/AZ 的静态值应接近 0（安装基线已去除）；
- 合加速度静态接近 0；如果希望保留 1g 重力，使用六面校准得到的零偏参数；
- 重启后 `[CALIB] loaded valid=1 ...`，参数保持；
- **阈值需重新标定**：仓库默认 `MAG > 150` 是“含重力”的旧语义；基线去除后静止值接近 0，
  建议先在测试面板观察静态噪声，再把阈值调到 30~50 左右（或按现场实测调整），
  否则振动检测会变得不敏感。

#### 六面校准（手动实验）

1. 将设备六个面分别朝上，每个面静止 2 秒，记录 AX/AY/AZ 原始值；
2. 对每轴：
   ```
   offset = (raw_pos + raw_neg) / 2
   scale  = 1000 * (raw_pos - raw_neg) / (2 * 1g_raw)
   ```
   其中 `1g_raw ≈ 2048`（±16G 量程，1g=2048 LSB）；
3. 把结果写入 `gateway.json`：
   ```json
   "calib": {"valid": 1, "imu_off": [ox, oy, oz],
             "imu_scale": [sx, sy, sz], "temp_off": 0}
   ```
4. 重启应用生效。

#### 温漂实验（后续 T3-C+）

- 记录常温、升温、降温三个阶段的静止 AX/AY/AZ 均值；
- 拟合“温度-零偏”曲线；
- 当前代码只支持固定 `temp_off`，分段温补系数留到后续版本。

#### 验收记录

| 日期 | 项目 | 结果 | 备注 |
|---|---|---|---|
| 2026-11-09 | T3-C 编译 | 通过 | |
| 2026-11-09 | 零偏采集 | 通过 | 静态基线 `off=[-2,1029,1750]`，`temp_off=0` |
| 2026-11-09 | 重启保持 | 通过 | `[CALIB] loaded valid=1` |

### 3.10 T3-B 内核准备（IIO buffer/trigger，已完成，2026-11-09）

当前内核 `.config`：

```
# CONFIG_IIO_BUFFER is not set
# CONFIG_IIO_TRIGGER is not set
```

目标：先只启用内核 IIO buffer/trigger（不改驱动），编译并验证系统启动；
驱动 buffer 化放到下一步，避免内核 + 驱动 + 应用同时变更。

```bash
export PATH=/opt/gcc-linaro-4.9.4-2017.01/bin:$PATH
export ARCH=arm
export CROSS_COMPILE=arm-linux-gnueabihf-

cd /home/szh/linux/kernel/linux-imx-rel_imx_4.1.15_2.1.0_ga_southbay

# 备份当前配置
cp .config .config.bak.iio

# 启用 IIO buffer/trigger
./scripts/config --enable IIO_BUFFER
./scripts/config --enable IIO_TRIGGER
./scripts/config --enable IIO_TRIGGERED_BUFFER
./scripts/config --enable IIO_SW_TRIGGER      # 可选：sysfs trigger
./scripts/config --enable IKCONFIG            # 可选：/proc/config.gz
./scripts/config --enable IKCONFIG_PROC

make olddefconfig

# 确认配置生效
grep -E "CONFIG_IIO_BUFFER|CONFIG_IIO_TRIGGER|CONFIG_IIO_TRIGGERED_BUFFER" .config

# 编译（按 CPU 核数调整 -j）
make -j$(nproc) zImage modules dtbs
```

部署：

```bash
# 备份板端当前内核/dtb
cp /home/szh/linux/tftp/zImage /home/szh/linux/tftp/zImage.bak
cp /home/szh/linux/tftp/imx6ull-southbay-emmc.dtb /home/szh/linux/tftp/imx6ull-southbay-emmc.dtb.bak

# 新内核/dtb
cp arch/arm/boot/zImage /home/szh/linux/tftp/
cp arch/arm/boot/dts/imx6ull-southbay-emmc.dtb /home/szh/linux/tftp/

# 模块（按实际内核模块目录）
make modules_install INSTALL_MOD_PATH=/home/szh/linux/nfs/rootfs
```

重启板子后验证：

```bash
uname -a
zcat /proc/config.gz | grep -E "IIO_BUFFER|IIO_TRIGGER|IIO_TRIGGERED_BUFFER"  # 若启用了 IKCONFIG_PROC
ls /sys/bus/iio/devices/
# 现有 ap3216c / icm20608 仍应存在，应用功能正常
```

预期：

- 内核启动正常，屏幕/触摸/网口/摄像头链路不受影响；
- `.config` 三项为 `y`；
- `/sys/bus/iio/devices/` 下设备正常；
- 此时 `icm20608` 驱动尚未注册 buffer，`iio:deviceX/buffer` 可能还不存在，属正常。

回退：

```bash
cp /home/szh/linux/tftp/zImage.bak /home/szh/linux/tftp/zImage
cp /home/szh/linux/tftp/imx6ull-southbay-emmc.dtb.bak /home/szh/linux/tftp/imx6ull-southbay-emmc.dtb
```

验收记录：

| 日期 | 项目 | 结果 | 备注 |
|---|---|---|---|
| 2026-11-09 | 启用 IIO buffer/trigger | 通过 | `CONFIG_IIO_BUFFER/TRIGGER/TRIGGERED_BUFFER/KFIFO_BUF=y` |
| 2026-11-09 | 内核启动 | 通过 | 4.1.15 #19，`iio:device1` buffer/trigger 正常 |

### 3.11 T3-B 驱动 buffer（已完成，2026-11-09）

**当前状态：驱动 buffer 与应用层均板端验证通过。**

板端已验证（2026-11-09）：

- `iio:device0/name = icm20608`；
- `buffer/`、`scan_elements/`、`trigger/` 均存在；
- `trigger/current_trigger = icm20608-dev2`；
- `buffer/length` 可从 2 调到 64；
- `hexdump -C -n 24 /dev/iio:device0` 连续输出数据；单帧实际 **16 字节**（7×s16 + padding，当前驱动无时间戳），`-n 24` 会显示 1.5 帧；
- `ap3216c.ko` 因内核升级后符号版本不匹配，重新编译后恢复正常。

已完成：

- 内核启用 `CONFIG_IIO_BUFFER` / `IIO_TRIGGER` / `IIO_TRIGGERED_BUFFER` / `IIO_KFIFO_BUF`；
- `icm20608.c` 增加 `scan_type`、`INDIO_BUFFER_TRIGGERED`、`iio_triggered_buffer_setup`、
  驱动内 hrtimer（100Hz）、trigger handler；
- 修复过一次 Oops：`iio_trigger_get()` 会访问 `trig->ops->owner`，
  已补 `icm20608_trigger_ops = { .owner = THIS_MODULE }`。

驱动/buffer 节点已验证。如需重新部署：

```bash
cd ~/shared/XRY/Edge-AI-Gateway
make -C core/icm20608
sudo cp build/module/icm20608.ko /home/szh/linux/nfs/rootfs/lib/modules/4.1.15/

# 板端
killall lvgl_gateway 2>/dev/null
rmmod icm20608 2>/dev/null
insmod /lib/modules/4.1.15/icm20608.ko
```

buffer 手工验证（ICM 可能是 `iio:device0` 或 `iio:device1`，按 `name` 确认）：

```bash
cat /sys/bus/iio/devices/iio:device0/name

DEV=/sys/bus/iio/devices/iio:device0
for f in $DEV/scan_elements/*_en; do echo 1 > $f; done
echo 64 > $DEV/buffer/length   # 容量，不是单帧长度
echo 1 > $DEV/buffer/enable
hexdump -C -n 24 /dev/iio:device0
hexdump -C -n 24 /dev/iio:device0
echo 0 > $DEV/buffer/enable
```

应用层验证（`dev_bridge` 已改为优先读 buffer）：

```bash
cd ~/shared/XRY/Edge-AI-Gateway
make lvgl
sudo cp build/lvgl/lvgl_gateway /home/szh/linux/nfs/rootfs/usr/bin/

# 板端
killall lvgl_gateway 2>/dev/null
/usr/bin/lvgl_gateway &
grep -E "IMU|SENSOR" /opt/edge-gateway/logs/gateway.log | tail -10
```

预期：

```
[INFO] [IMU] buffer enabled: /dev/iio:device0 (100Hz)
```

> 单帧实际 16 字节（7×s16 + padding）；应用 `read()` 返回 16 字节时正常解析，
> 不再按 24 字节（含时间戳）判断。

测试面板的 IMU 数值应明显平滑、刷新更快；如果 buffer 打开失败，会回退 sysfs 并打印 `fallback sysfs`。

失败排查：

```bash
dmesg | tail -40
cat $DEV/trigger/current_trigger 2>/dev/null
cat $DEV/buffer/length
```

验收记录：

| 日期 | 项目 | 结果 | 备注 |
|---|---|---|---|
| 2026-11-09 | 驱动编译 | 通过 | `trig->ops` 修复后编译通过 |
| 2026-11-09 | buffer 节点 | 通过 | `buffer/scan_elements/trigger` + `current_trigger` |
| 2026-11-09 | buffer 读数据 | 通过 | `hexdump` 24 字节连续变化 |
| 2026-11-09 | 应用层 100Hz | 通过 | `[IMU] buffer enabled: /dev/iio:device1 (100Hz)`，无 read 错误 |
| 2026-11-10 | 故障注入（unbind SPI） | 通过 | `buffer read -1` → `COMM_ERR` → `bind` → `buffer enabled` + `recovered` |

### 3.12 T4 摄像头误报抑制（已完成，2026-11-09）

**当前状态：板端三场景验证通过（静态画面 / 开关灯 / 行人走过）。**

实现内容（`cam_feed.c` 运动检测）：

- 滑动平均背景建模（默认 α=0.05）；
- 双层光照突变抑制：
  - 背景均值突变：相对阈值（15%）+ 绝对下限 `MD_LIGHT_MIN=10`；
  - 相邻帧均值突变：`MD_LIGHT_FRAME_JUMP=10`，开关灯瞬间即使累计差未超阈值也能立即识别；
- 光照事件后跳过 `MD_LIGHT_SKIP_FRAMES=20` 帧（约 1.3s@15fps），跳过期间持续用当前帧重建背景，
  等 OV5640 自动曝光/白平衡稳定；跳过期间不产生命中，只在事件开始时打印一次日志；
- 3×3 开运算（腐蚀+膨胀）去除孤立噪点；
- 4 邻域连通域，过滤面积 <20 的碎块；
- 保留原有连续 2 次确认 + 冷却节流。

参数：

| 参数 | 默认 | 接口 / 宏 |
|---|---|---|
| 灰度差阈值 | 24 | `cam_feed_set_motion_diff()`（UI 已有） |
| 前景占比 | 5% | `cam_feed_set_motion_threshold()`（UI 已有） |
| 背景更新系数 | 0.05 | `cam_feed_set_motion_bg_alpha(50)` |
| 最小连通域面积 | 20 | `cam_feed_set_motion_min_area(20)` |
| 亮度突变阈值（相对） | 15% | `cam_feed_set_motion_light_jump(15)` |
| 亮度突变绝对下限 | 10（0~63） | `MD_LIGHT_MIN` |
| 单帧均值突变阈值 | 10（0~63） | `MD_LIGHT_FRAME_JUMP` |
| 光照后跳过帧数 | 20 | `MD_LIGHT_SKIP_FRAMES` |
| 转换/预览帧间隔 | 50ms（约 15~20fps） | `CAM_FPS_MS` |

编译：

```bash
cd ~/shared/XRY/Edge-AI-Gateway
make lvgl
sudo cp build/lvgl/lvgl_gateway /home/szh/linux/nfs/rootfs/usr/bin/
```

板端启动：

```bash
killall lvgl_gateway
sleep 1
GATEWAY_LOG_LEVEL=debug /usr/bin/lvgl_gateway &
sleep 3
grep -E "CAM" /opt/edge-gateway/logs/gateway.log | tail -20
```

| 场景 | 预期 | 实测 |
|---|---|---|
| 静态画面 30 秒 | 不产生 `motion hit` | 通过 |
| 开灯 / 关灯 | 只出现 `light jump reset`，不产生 `motion hit` | 通过 |
| 人走过画面 | 出现 `motion hit area=...` | 通过（area 200~380/4000） |

板端实测日志（2026-11-09）：

```
[INFO]  [CAM] streaming 640x480 YUYV from /dev/video0 (bytesperline=1280 sizeimage=614400)
[INFO]  [CAM] PLL 0x3035=0x11 applied (30fps workaround)
[DEBUG] [CAM] sensor=31 fps, conv=15 fps, conv_ms=24.3
[DEBUG] [CAM] light jump reset: mean 2 -> 15 (bg_jump=13 frame_jump=9 thr=10)
[DEBUG] [CAM] light jump reset: mean 33 -> 23 (bg_jump=10 frame_jump=13 thr=10)
[DEBUG] [CAM] motion hit area=312/4000
```

> 开灯后的约 1.3 秒内（AE 稳定期）真实运动会被忽略，这是抑制误报的代价；需要更灵敏时
> 可把 `MD_LIGHT_SKIP_FRAMES` 降到 12。
>
> 暗场景下若 `bg_mean < 5`（画面几乎全黑），调大 `MD_LIGHT_MIN` 或补充环境光。

验收记录：

| 日期 | 项目 | 结果 | 备注 |
|---|---|---|---|
| 2026-11-09 | T4 编译 | 通过 | `/dev/video0` OV5640 probe 正常 |
| 2026-11-09 | 静态 30 秒 | 通过 | 无 `motion hit` |
| 2026-11-09 | 开关灯 | 通过 | 仅 `light jump reset`，无 `motion hit` |
| 2026-11-09 | 行人检出 | 通过 | `motion hit area=296~312/4000` |
| 2026-11-10 | 静态 13 分钟（新二进制） | 通过 | `02:05` 启动后无 `motion hit`、无 `light jump reset` |
| 2026-11-10 | 开关灯 / 行人复测 | 通过 | 仅 `light jump reset (thr=10)`；行人 `motion hit area=2xx/4000` |

### 3.13 T6 断网缓存 + MQTT 重连（已完成，2026-11-10）

**当前状态：缓存、非阻塞连接、独立落盘线程、自动补传均板端验证通过。**

实现：

- 设备 ID：`gw-<eth0 MAC>`，topic `gateway/{device_id}/event`；
- 事件先写 `/opt/edge-gateway/data/queue/events.jsonl`（600，tmp+fsync+rename）；
- 常驻网络线程 + 有界队列（64 条），指数退避 1s→60s；
- 发送成功按 `id` 删除本地缓存；
- 连接恢复或队列有空位时 `reload_store()` 补传；
- 空闲 30s 发 PINGREQ 保活。

编译：

```bash
cd ~/shared/XRY/Edge-AI-Gateway
make lvgl
```

启动：

```bash
killall lvgl_gateway 2>/dev/null
/usr/bin/lvgl_gateway &
tail -20 /opt/edge-gateway/logs/gateway.log
```

预期：

```
[MQTT] start client=gw-xxxxxxxxxxxx pending=0 broker=192.168.3.100:1883
```

#### 断网缓存

```bash
ifconfig eth0 down

# 触发事件：按键、改设置、告警等
wc -l /opt/edge-gateway/data/queue/events.jsonl
tail -3 /opt/edge-gateway/data/queue/events.jsonl

ifconfig eth0 up
# 等待 1~60s（指数退避）
wc -l /opt/edge-gateway/data/queue/events.jsonl
```

预期：

- 断网期间条数增加；
- 网络恢复且 broker 可达后条数下降，最终为 0；
- 如果 broker 不可达（`192.168.3.100:1883` 没起），条数保持不变，说明事件没有丢。

#### 重启补传

```bash
killall lvgl_gateway
/usr/bin/lvgl_gateway &
grep "MQTT.*start" /opt/edge-gateway/logs/gateway.log | tail -1
```

预期 `pending=N`（N 为重启前未发送条数），broker 可达后自动补传。

#### 故障排查

```bash
ping -c 1 192.168.3.100
# 有 nc 时：
nc -z 192.168.3.100 1883

tail -20 /opt/edge-gateway/logs/gateway.log
ls -l /opt/edge-gateway/data/queue/
```

- 队列路径回退：日志 `[EVENT] queue ...`；
- 网络不可达：日志/队列条数不下降，属预期；
- 事件重复：本地按 `id` 去重；云端 QoS0 未做服务端幂等，后续可在 broker/服务端补。

验收记录：

| 日期 | 项目 | 结果 | 备注 |
|---|---|---|---|
| 2026-11-09 | T6 编译 | 通过 | |
| 2026-11-09 | 断网缓存 | 通过 | broker 不可达时 `events.jsonl` 持续增长不丢（13 → 34 条） |
| 2026-11-10 | 缓存增长/不丢 | 通过 | 启动 `pending=57` → 4 个传感器事件后 `63`，4 分钟内保持 63 不变 |
| 2026-11-09 | 非阻塞连接 | 通过 | 非阻塞 connect + 2s 超时，broker 不可达不再拖住网络线程/触发看门狗 |
| 2026-11-09 | UI 不阻塞 | 通过 | 事件落盘改独立 writer 线程，关闭云端复核时触摸正常 |
| 2026-11-09 | 重启补传 | 通过 | `pending=2` → 连上后队列归 0 |
| 2026-11-10 | 恢复补传（实测） | 通过 | 停 broker 期间队列稳定 4 条不丢；恢复后 60s 内补传完毕归 0 |
| 2026-11-10 | 重启补传（实测） | 通过 | 断网时重启，日志 `pending=2`；broker 恢复后归 0 |

> 补传验证前置条件：VM 或局域网内启动 mosquitto，并用 `GATEWAY_MQTT_BROKER=192.168.3.26`
> 覆盖 broker 地址后启动应用。

### 3.14 T8 单实例 + 看门狗 + 健康心跳（已完成，2026-11-09）

**当前状态：单实例、看门狗、异常复位均板端验证通过。**

实现：

- 单实例锁：`/tmp/lvgl_gateway.lock`（`O_CREAT|O_EXCL` 写入 pid；进程已退出 but 锁残留时按 pid 存活判定回收，避免 `flock` 在 NFS 上不可靠）；
- 看门狗：`/dev/watchdog`，30s 超时；`GATEWAY_WATCHDOG=0` 可禁用；
- 心跳：MAIN（主循环 500ms）、BRIDGE（200ms 定时器）、CAM（摄像头每秒）、NET（网络线程每秒）；
- 任一已启动模块心跳超时 → 停止喂狗 → 30s 后系统复位；
- 摄像头未启动时不参与判断，避免未接线导致复位。

编译：

```bash
cd ~/shared/XRY/Edge-AI-Gateway
make lvgl
```

#### 单实例

```bash
killall lvgl_gateway 2>/dev/null
/usr/bin/lvgl_gateway &
sleep 1
/usr/bin/lvgl_gateway
```

预期第二个进程输出：

```
[ERROR] [HEALTH] another instance is running (...)
[WARN]  [MAIN] another instance is running, exit
```

并立即退出。

#### 看门狗状态

```bash
ls -l /dev/watchdog
tail -20 /opt/edge-gateway/logs/gateway.log | grep HEALTH
```

预期：

```
[INFO] [HEALTH] watchdog enabled, timeout=30s
```

如果 `/dev/watchdog` 不存在：

```
[WARN] [HEALTH] /dev/watchdog open failed: ... (watchdog disabled)
```

不影响应用运行。

#### 主线程卡死（危险，会复位板子）

```bash
kill -STOP $(pidof lvgl_gateway)
# 约 30s 后看门狗应复位系统
```

> 仅在确认可以重启时执行。

#### 正常退出

```bash
killall -TERM lvgl_gateway
```

预期：

```
[INFO] [MAIN] application exit
[LOG] closed (dropped=0)
```

锁文件应被删除：

```bash
ls /var/run/lvgl_gateway.lock 2>/dev/null || echo "lock released"
```

验收记录：

| 日期 | 项目 | 结果 | 备注 |
|---|---|---|---|
| 2026-11-09 | T8 编译 | 通过 | |
| 2026-11-09 | 单实例 | 通过 | `O_EXCL` + pid 存活检查；第二实例 exit=1 |
| 2026-11-09 | 看门狗 | 通过 | `/dev/watchdog` 30s 启用 |
| 2026-11-09 | 异常复位 | 通过 | `kill -9` 未 magic close，30s 后 watchdog 复位 |

### 3.15 T9 应用级 OTA（已完成，2026-11-10）

**当前状态：网络 OTA 板端全流程验证通过（安装 → 重启 → 确认 → 无更新 → 回滚）。**

实现：

- 目录：`/opt/edge-gateway/releases/<version>/`、`current` 符号链接；
- 包：外层 `pkg-<version>.tar.gz`，内含 `manifest.json` + `payload.tar.gz` + `checksums.sha256`；
- 网络层：`http_get.c` 最小 HTTP/1.1 GET（裸 TCP + 非阻塞 connect + 超时，仅 `http://`）；
- 检查：`ota_check(manifest_url)` → 版本比较（按数字段，`1.10 > 1.9`）→ 下载 → sha256 校验
  → 解包 → `ota_install()`；
- 触发：`GATEWAY_OTA_URL`（启动时检查，拉取式）；`GATEWAY_OTA_INSTALL`（离线/工厂模式）；
- 生效：安装成功后 `sync + reboot`，`GATEWAY_OTA_NO_REBOOT=1` 可只装不重启；
- 状态机：`INSTALLING → PENDING_CONFIRM → CONFIRMED`，启动未确认/安装中断自动回滚；
- 依赖板端命令：`sha256sum`、`tar`、`gzip`、`cp`、`rm`（Buildroot BusyBox）。
- **BusyBox 注意**：该版本 `tar` 未编入 gzip 支持（`-z`/`-a` 都不可用），
  `.tar.gz` 统一走 `gzip -dc 'x.tar.gz' | tar -xf - -C dir`。

编译：

```bash
cd ~/shared/XRY/Edge-AI-Gateway
make lvgl
```

#### 服务器：造包 + HTTP 服务（VM）

统一入口在 `cloud/`（见 `cloud/README.md`）：

```bash
cd ~/shared/XRY/Edge-AI-Gateway/cloud
./cloudctl.sh start            # 精判+OTA(8000) + MQTT(1883)
./make_ota_pkg.sh 1.1.0        # 造包到 cloud/ota/（默认用 ../build/lvgl/lvgl_gateway）
cat ota/manifest.json
```

> 旧方式（`python3 -m http.server 8000` + `~/ota-srv`）已由 `cloudctl.sh` 取代；
> OTA 与云端精判共用 **8000** 端口，不再冲突。

#### 板端：基线 + 触发

```bash
mkdir -p /opt/edge-gateway/releases/1.0.0
cp /usr/bin/lvgl_gateway /opt/edge-gateway/releases/1.0.0/lvgl_gateway
chmod +x /opt/edge-gateway/releases/1.0.0/lvgl_gateway
ln -sfn releases/1.0.0 /opt/edge-gateway/current

killall lvgl_gateway; sleep 1
GATEWAY_OTA_URL=http://192.168.3.26:8000/ota/manifest.json \
  /opt/edge-gateway/current/lvgl_gateway &
```

实测日志（2026-11-10，1.0.0 → 1.1.0）：

```
[OTA] check http://192.168.3.26:8000/ota/manifest.json
[HTTP] downloaded .../manifest.json -> /opt/edge-gateway/ota/staging/download/manifest.json
[OTA] download http://192.168.3.26:8000/ota/pkg-1.1.0.tar.gz (current=1.0.0 remote=1.1.0)
[HTTP] downloaded .../pkg-1.1.0.tar.gz -> .../pkg.tar.gz
[OTA] installed 1.1.0 (previous=1.0.0), pending confirm
[OTA] update 1.1.0 ready (previous=1.0.0)
[MAIN] OTA installed, rebooting to apply
imx2-wdt: Device shutdown: Expect reboot!
```

重启后（rcS 需指向 `current`）：

```
[OTA] init root=/opt/edge-gateway state=PENDING_CONFIRM current=1.1.0
[OTA] version 1.1.0 confirmed
```

无更新：

```
[OTA] up to date (current=1.1.0, remote=1.1.0)
```

回滚（把 `state.json` 改成 `PENDING_CONFIRM` + `boot_count=1` 再启动）：

```
[OTA] new version not confirmed after boot, rolling back
[OTA] rolled back to 1.0.0
```

#### 包完整性认证（T11-①，HMAC-SHA256）

板端没有 `openssl` 命令，因此采用**内置 HMAC-SHA256**（自包含实现，
`core/lvgl/src/ota_hmac.c`），服务端用同一密钥签名，板端校验，失败拒绝安装。

```bash
cd cloud
./gen_ota_keys.sh          # 一次性：生成 secrets/ota.key（32 字节随机密钥）
./make_ota_pkg.sh 1.4.0    # 自动计算 HMAC，manifest 增加 "hmac" 字段
```

密钥部署（`make deploy` 会自动做，权限 600）：

```bash
sudo mkdir -p /home/szh/linux/nfs/rootfs/opt/edge-gateway/ota
sudo cp cloud/secrets/ota.key /home/szh/linux/nfs/rootfs/opt/edge-gateway/ota/
sudo chmod 600 /home/szh/linux/nfs/rootfs/opt/edge-gateway/ota/ota.key
```

校验逻辑（`ota_mgr.c: verify_hmac`）：

- 密钥：`GATEWAY_OTA_HMAC_KEY` 或 `/opt/edge-gateway/ota/ota.key`；
- manifest 无 `hmac` 字段时默认**拒绝**；
- 调试可 `GATEWAY_OTA_REQUIRE_SIG=0` 放行无认证包（仅限开发）。

负向测试：

```bash
# ① 篡改包（sha256 也会变，最先被拦）：
printf 'x' >> cloud/ota/pkg-1.4.0.tar.gz
# ② 只改坏 hmac（包不变）：
#    手改 manifest 的 hmac 前几位，再触发 OTA
```

预期板端：

```
[OTA] hmac mismatch: got=... want=...
[OTA] hmac check failed, reject 1.4.0
```

已知局限：

- 对称认证：设备被攻破会泄露密钥；生产建议 mbedTLS/openssl 的 RSA/ECDSA 验签；
- manifest 走 HTTP，未加密；HMAC 防篡改但不防窃听（TLS 见 T11-③）；
- 密钥文件在 `/opt`（600），root 可替换；更严格的做法是编译进固件或配合 secure boot。

验收记录（认证）：

| 日期 | 项目 | 结果 | 备注 |
|---|---|---|---|
| 2026-11-10 | SHA-256/HMAC 实现 | 完成 | `ota_hmac.c`，流式文件校验，无外部依赖 |
| 2026-11-11 | 标准向量自测 | 通过 | `sha256(abc)=ba7816bf…`、`hmac(key,fox)=f7bc83f4…` 与标准值一致 |
| 2026-11-11 | 密钥生成/签名包 | 通过 | `gen_ota_keys.sh` + `make_ota_pkg.sh` 输出 `hmac` 字段 |
| 2026-11-11 | 正常包安装 | 通过 | `[OTA] hmac ok` → 1.4.0 → 1.5.0 |
| 2026-11-11 | hmac 被篡改 | 通过 | `hmac mismatch` → `reject 1.6.0`，`current` 保持 1.5.0 |
| 2026-11-11 | manifest 缺 hmac | 通过 | `manifest missing "hmac" (require=1), reject` |

#### 已知局限

- 只支持 `http://`，无 TLS；只校验包 sha256，**未做签名验签**（T11）；
- 只在启动时检查一次，定时拉取 / MQTT 下发触发尚未接入；
- 若新版本**根本启动不了**（`ota_init()` 都跑不到），`boot_count` 不会增长，不会自动回滚，
  需要 rcS 重试计数或 U-Boot A/B。

验收记录：

| 日期 | 项目 | 结果 | 备注 |
|---|---|---|---|
| 2026-11-10 | HTTP 下载 + sha256 | 通过 | manifest/包下载、外层 sha256 校验 |
| 2026-11-10 | 安装 + 切换 | 通过 | `installed 1.1.0`，`current -> releases/1.1.0` |
| 2026-11-10 | 重启确认 | 通过 | `PENDING_CONFIRM` → `version 1.1.0 confirmed`，`state=3` |
| 2026-11-10 | 无更新 | 通过 | `up to date`，不重复下载 |
| 2026-11-10 | 回滚 | 通过 | `boot_count=1` → `rolled back to 1.0.0` |
| 2026-11-11 | 真实故障回滚 | 通过 | 2.4.0 启动两次崩溃未确认 → 自动回滚 2.2.0 |
| 2026-11-10 | BusyBox 兼容 | 通过 | `tar` 无 gzip 支持，改 `gzip -dc \| tar -xf -` |
| 2026-11-10 | 定时拉取 | 通过 | `GATEWAY_OTA_INTERVAL_MIN=1` 时每 60s `periodic check` |
| 2026-11-10 | MQTT 触发 | 通过 | `{"cmd":"ota",...}` → `cmd check`，安装后自动重启（1.1.0→1.2.0→1.3.0） |
| 2026-11-10 | 后台安装后重启 | 通过 | `OTA installed by background check, restarting`，重启后 `state=3` |

### 3.16 T5 事件总线 + 告警状态机（待验证）

**当前状态：代码已完成，待编译/板端验证。**

新增：

- `src/event_bus.c/h`：64 条有界队列，P0~P4 优先级，线程安全；
- `src/alarm_fsm.c/h`：4 分区状态机
  `NORMAL → PENDING → ACTIVE → MUTED → RECOVERY → NORMAL`；
- `dev_bridge.c`：告警触发/消警统一走 `alarm_fsm_dispatch`，并发布 `EV_ALARM`；
- `ui_home.c`：长按消警/详情消警同步 `ALARM_EV_ACK`。

状态迁移：

| 当前 | 事件 | 下一状态 |
|---|---|---|
| NORMAL | TRIGGER | PENDING |
| PENDING | CONFIRM/TRIGGER | ACTIVE |
| PENDING | ACK/CLEAR/TIMEOUT | NORMAL |
| ACTIVE | ACK/MUTE | MUTED |
| ACTIVE | CLEAR | RECOVERY |
| ACTIVE | TRIGGER | ACTIVE |
| MUTED | CLEAR | RECOVERY |
| MUTED | TRIGGER | ACTIVE |
| MUTED | TIMEOUT | NORMAL |
| RECOVERY | CLEAR/TIMEOUT | NORMAL |
| RECOVERY | TRIGGER | PENDING |

编译：

```bash
cd ~/shared/XRY/Edge-AI-Gateway
make lvgl
```

板端验证（待恢复）：

1. 触发传感器/陌生人告警 → 时间轴/弹窗正常，日志 `[DEBUG] [EVENT] alarm ...`；
2. 长按消警 → 告警解除，FSM 回 NORMAL；
3. 云端白名单自动消警 → FSM 进 RECOVERY，再触发可重新进入 PENDING/ACTIVE；
4. 反复触发/消警 10 次，状态不粘滞。

当前线程重构（atomic/有界队列/优雅退出/栈大小）**尚未做**，留到 T5-B。

验收记录：

| 日期 | 项目 | 结果 | 备注 |
|---|---|---|---|
| 2026-11-09 | T5 编译 | 待验证 | |
| 2026-11-09 | 告警触发/消警 | 待验证 | |
| 2026-11-09 | 状态机迁移 | 待验证 | |

### 3.17 T7/T10 在线状态 + 远程配置（已完成，2026-11-10）

**当前状态：板端 + VM mosquitto 2.0.18 全链路验证通过（上线/WiIl/远程配置/ACK/补传）。**

实现：

- CONNECT 增加 Will/LWT：`gateway/{device_id}/status`，异常断开自动发 `{"online":false}`；
- 连接成功后发布 `{"online":true,"device_id":...,"ts":...}`，并订阅 `gateway/{device_id}/cmd`；
- 下行命令：`set_config`、`set_warn`、`get_config`；
- ACK：`gateway/{device_id}/cmd/ack`；
- 远程配置写入 `gateway.json`，`dev_bridge` 每 1s 同步一次并刷新 UI；
- `mqtt_broker` 字段持久化（`gateway.json`）：优先级 **环境变量 > 配置 > 编译默认**；
  可用 `set_config` 远程写入，下次启动生效（`get_config` 也会返回该字段）；
- **T11-② 用户名/密码**：`mqtt_user` / `mqtt_pass` 持久化，CONNECT 报文携带
  （flags bit7/bit6）；broker 侧 `allow_anonymous false` + `password_file`；
- **T11-B2 HTTP 鉴权**：`cloud_api_key` 持久化，`X-API-Key` 覆盖 `/detect`、`/ota/*`、
  `/whitelist*`；`/health` 保持开放（巡检用）；
- **T11-B3 设备影子**：`gateway/{id}/shadow/reported` 上报实际配置（不含密码/Key），
  连接后与配置变更（`config_generation` 变化）时自动发布；
- 空闲 30s 发 PINGREQ；断连后 5s 起按 1s→60s 指数退避重连（空闲也重连，保持在线状态）。

#### 本轮修复的问题（验证中发现）

| 问题 | 原因 | 修复 |
|---|---|---|
| 每 30s 出现 `NET heartbeat timeout` | `mqtt_ping()` 阻塞等 PINGRESP，但 PINGRESP 已被 `mqtt_read_incoming()` 先消费 → 必然 800ms 超时 → 主动断连 | 改为只发 PINGREQ，收包统一由 `mqtt_read_incoming()` 处理 |
| 断连且队列为空时心跳停止 | 线程卡在 `pthread_cond_timedwait` 内层循环，既不心跳也不重连 | 每次等待后 `health_beat(NET)`；未连接时每 5s 回外层重连 |
| MQTT 命令延迟 30 秒 | 空闲等待 1s，且只有 ping 到点（30s）才回到循环顶部读包 | 等待降到 200ms，且等待期间也调用 `mqtt_read_incoming()` |

编译：

```bash
cd ~/shared/XRY/Edge-AI-Gateway
make lvgl
```

#### 验证环境

VM 起 mosquitto（监听 0.0.0.0 + 允许匿名，仅内网测试）：

```bash
sudo systemctl stop mosquitto 2>/dev/null
cat > /tmp/mosq.conf <<'EOF'
listener 1883 0.0.0.0
allow_anonymous true
EOF
nohup mosquitto -c /tmp/mosq.conf -v > /tmp/mosq.log 2>&1 &
```

板端指定 broker：

```bash
killall lvgl_gateway; sleep 1
GATEWAY_MQTT_BROKER=192.168.3.26 GATEWAY_LOG_LEVEL=debug \
  /opt/edge-gateway/current/lvgl_gateway &
```

#### 订阅/查看

```bash
mosquitto_sub -h 127.0.0.1 -t 'gateway/#' -v
```

#### 下发配置（实测）

```bash
DEVICE=gw-b8ae1d010000

mosquitto_pub -h 127.0.0.1 -t "gateway/$DEVICE/cmd" -m '{"cmd":"get_config","id":1}'
mosquitto_pub -h 127.0.0.1 -t "gateway/$DEVICE/cmd" -m '{"cmd":"set_config","id":2,"motion_diff":24}'
mosquitto_pub -h 127.0.0.1 -t "gateway/$DEVICE/cmd" -m '{"cmd":"set_warn","id":3,"zone":0,"threshold":50,"enabled":1}'
mosquitto_pub -h 127.0.0.1 -t "gateway/$DEVICE/cmd" -m '{"cmd":"unknown","id":5}'
```

实测应答（订阅端）：

```
cmd      {"cmd":"get_config","id":1}
cmd/ack  {"cloud_review":1,"motion_interval":1,"motion_diff":20,"brightness":100,"schedule_on":0}

cmd      {"cmd":"set_config","id":2,"motion_diff":24}
cmd/ack  {"id":2,"cmd":"set_config","ok":true}

cmd      {"cmd":"set_warn","id":3,"zone":0,"threshold":50,"enabled":1}
cmd/ack  {"id":3,"cmd":"set_warn","ok":true}

cmd      {"cmd":"unknown","id":5}
cmd/ack  {"id":5,"cmd":"unknown","ok":false}
```

板端确认：

```bash
grep "cmd=" /opt/edge-gateway/logs/gateway.log | tail -5
grep -n "threshold" /opt/edge-gateway/config/gateway.json
```

> `get_config` 的 ack 是**裸配置 JSON**（无 `id`），`set_*`/未知命令的 ack 才是
> `{"id":..,"cmd":..,"ok":..}`，属当前设计。

#### 设备影子（T11-B3）

连接后/配置变更时自动上报：

```
gateway/gw-<id>/shadow/reported
{"device_id":"...","ts":...,"version":"2.0.0","reported":{
  "cloud_review":1,"motion_interval":1,"motion_diff":24,
  "brightness":80,"schedule_on":0,
  "sched_start_h":22,"sched_end_h":6,
  "debounce":2,"cooldown_sec":60,
  "mqtt_broker":"192.168.3.26","mqtt_user":"gateway"}}
```

验证：

```bash
mosquitto_sub -h 127.0.0.1 -u gateway -P '<密码>' -t 'gateway/#' -v
```

重启应用或执行一次 `set_config` 后，应看到新的 `shadow/reported`。
板端日志：`[MQTT] shadow reported (gen=N)`。

#### 用户名密码认证（T11-②）

broker 侧（`cloud/mosquitto.conf`）：

```
allow_anonymous false
password_file secrets/mosquitto.passwd
```

生成账号：

```bash
cd cloud
./gen_mqtt_passwd.sh gateway 'Gw#2026pass'
./cloudctl.sh restart
```

板端配置（broker 仍匿名时先下发，或直接写 `gateway.json`）：

```bash
mosquitto_pub -h 127.0.0.1 -t 'gateway/gw-<id>/cmd' \
  -m '{"cmd":"set_config","id":16,"mqtt_user":"gateway","mqtt_pass":"Gw#2026pass"}'
```

重启应用后日志：

```
[MQTT] auth enabled (user=gateway, pass=***)
[MQTT] connected 192.168.3.26:1883, subscribe+online
```

错误密码时只有 `connect ... failed` 重试，不会上线。启用认证后命令行工具要带 `-u/-P`。

> 密码在 `gateway.json` 明文（600 权限）；MQTT 传输仍为明文，TLS 见 T11-③。

#### LWT（异常断开）

```bash
killall lvgl_gateway; sleep 1
GATEWAY_WATCHDOG=0 GATEWAY_MQTT_BROKER=192.168.3.26 /opt/edge-gateway/current/lvgl_gateway &
sleep 3
kill -9 $(pidof lvgl_gateway)
```

订阅端实测：

```
gateway/gw-b8ae1d010000/status {"online":false}
```

（不含 `device_id`/`ts`，说明是 broker 代发的 Will，而不是应用优雅下线的消息。）

#### 注意

- 当前 MQTT 无用户名/密码/TLS，**仅适用于内网验证**；T11 安全阶段补 TLS/mTLS/ACL；
- 如果 broker 不可达，命令不会下发，设备继续本地运行并缓存事件；
- `device_id` 来自 eth0 MAC，可用 `grep "MQTT.*start" gateway.log` 查看。

验收记录：

| 日期 | 项目 | 结果 | 备注 |
|---|---|---|---|
| 2026-11-10 | 上线状态/LWT | 通过 | `{"online":true,...}`；`kill -9` 后 broker 代发 `{"online":false}` |
| 2026-11-10 | 命令下发/ACK | 通过 | `set_config`/`set_warn`/`unknown` 均在 200ms 内回 ACK |
| 2026-11-10 | get_config | 通过 | 返回当前配置 JSON |
| 2026-11-10 | 配置落盘 | 通过 | `gateway.json` 中 `threshold` 30→50→30、`motion_diff` 20→24 |
| 2026-11-10 | ping/心跳修复 | 通过 | 120s 观察无 `NET heartbeat timeout`，重连次数不再增长 |
| 2026-11-10 | 命令延迟修复 | 通过 | 30s → 200ms 内响应 |
| 2026-11-11 | 告警触发/自动消警 | 部分通过 | `alarm zone=0 code=1 src=STRANR` → 云端判定无人员 `code=0`；人工消警/超时待验 |
| 2026-11-10 | `mqtt_broker` 持久化 | 通过 | `set_config` 写入后，无环境变量重启仍连 `192.168.3.26` |
| 2026-11-11 | 用户名/密码认证 | 通过 | `auth enabled` + `connected`；错误密码仅重试不上线 |
| 2026-11-11 | HTTP 鉴权（X-API-Key） | 通过 | 无 Key→401；带 Key→200；OTA 下载正常 |
| 2026-11-11 | 设备影子 reported | 通过 | 连接 `gen=0`；改配置 `gen=1/2` 自动重发；无敏感字段 |
| 2026-11-11 | 统一错误码 | 通过 | 坏包日志 `[OTA:HMAC]`，可按模块检索 |

---

### 3.18 采集线程卡死（KEY0 evdev 阻塞）修复（已完成，2026-11-09）

**现象**：运行一段时间后 `app` 报 `AP3216C fault: STALE`，且**永不恢复**；同时 IMU 仍正常
（IMU 走独立 buffer 线程），界面光照/距离数值停止刷新。

**根因**：`dev_bridge.c` 的 `find_key_dev()` 以阻塞模式打开 `/dev/input/eventX`：

```c
int fd = open(path, O_RDONLY);          /* 阻塞模式 */
...
if (poll(&pfd, 1, POLL_MS) > 0) {
    while (read(key_fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) { ... }
}
```

按一次 KEY0 后，`poll` 返回可读，内层 `while` 把队列读完后，下一次 `read()` **永久阻塞**，
采集线程再也回不到主循环 → 不再调用 `sensor_mgr_update_ap()` → `last_ms` 不更新 →
UI 定时器 `sensor_mgr_check_stale()` 判超时 → `SENSOR_STALE`。

**证据**（板端 `/proc/<pid>/task/*/wchan`）：

```
168 lvgl_gateway state=R wchan=vb2_internal_dqbuf             # 摄像头线程（正常）
169 lvgl_gateway state=S wchan=iio_buffer_read_first_n_outer   # IMU buffer 线程（正常）
170 lvgl_gateway state=S wchan=evdev_read                      # 采集线程（卡死）
```

**修复**：

- `find_key_dev()` 改为 `open(path, O_RDONLY | O_NONBLOCK)`，内层 `read()` 读空返回
  `EAGAIN`，循环自然退出；
- 按键处理增加 DEBUG 日志：`[BRIDGE] KEY0 event value=<0|1>`；
- 顺带修复 `sensor_mgr.c`：有效样本时清零 `invalid_cnt`（原来累计失败 3 次后长期过度敏感）。

编译部署：

```bash
cd ~/shared/XRY/Edge-AI-Gateway
make lvgl
sudo cp build/lvgl/lvgl_gateway /home/szh/linux/nfs/rootfs/usr/bin/
```

验证：

```bash
killall lvgl_gateway
sleep 1
GATEWAY_LOG_LEVEL=debug /usr/bin/lvgl_gateway &
sleep 3
# 连续按 KEY0 5 次（间隔 1 秒），等 8 秒
grep -E "SENSOR|KEY0" /opt/edge-gateway/logs/gateway.log | tail -20
```

预期：

```
[DEBUG] [BRIDGE] KEY0 event value=1
[DEBUG] [BRIDGE] KEY0 event value=0
...（每次按键一对）
```

且**不再出现新的 `AP3216C fault: STALE`**；采集线程 `wchan` 应始终保持 `poll_schedule_timeout`。

验收记录：

| 日期 | 项目 | 结果 | 备注 |
|---|---|---|---|
| 2026-11-09 | 根因定位 | 通过 | `wchan=evdev_read`，同刻 AP3216C STALE |
| 2026-11-09 | `O_NONBLOCK` 修复 | 通过 | 9 次按键后采集线程存活，49 秒无 STALE |
| 2026-11-10 | 修复后复测（新二进制） | 通过 | 按键正常、无 STALE；AP3216C 注入 `COMM_ERR` → `recovered` |
| 2026-11-10 | `invalid_cnt` 清零 | 通过 | 故障→恢复后无“单次异常即升级”抖动 |

> `invalid_cnt` 清零改变了 T3-A 的故障判定节奏，已按 3.5 回归确认：`rmmod` → `COMM_ERR` →
> `insmod` → `reconnected` + `recovered`；SPI `unbind`/`bind` 注入同样通过。

### 3.19 T5-B 线程模型/同步重构（已完成，2026-11-10）

**当前状态：代码完成并板端验证通过（功能回归 + 54ms 优雅退出 + 可立即重启）。**

改动清单：

| 模块 | 改动 | 目的 |
|---|---|---|
| `cam_feed.c` | 帧交接加 `s_frame_lock`（发布索引 + 428KB memcpy 在锁内）；`s_motion_hits` 改 `__atomic` 自增/取走；线程由 detach 改为可 join；新增 `cam_feed_stop()` | 消除“读到半帧”与命中计数丢失；退出可回收 |
| `cam_feed.c` | 转换线程 `nice+5` | 单核下优先保证 UI 渲染响应 |
| `detector.cpp` | 单槽任务 → **有界队列（深度 2）**，每任务独立帧快照；队列满丢弃并计数；结论交接（zone/result/stage/name）改 `pthread_mutex` 保护；工作线程可 join，新增 `detector_stop()` | 修复“后一次提交覆盖前一任务帧/zone”的错配；多字段结论不再依赖 volatile 碰运气 |
| `detector.cpp` | `s_hits` 改 `__atomic` | 统计与展示竞争 |
| `dev_bridge.c` | IMU 线程 `poll(200ms)` 后再 `read`；采集/IMU 线程可 join；新增 `dev_bridge_stop()`（顺带关闭 LED/BEEP/PWM fd） | `read` 不再无限阻塞，退出 300ms 内完成 |
| `dev_bridge.c` | `g_imu_buffer_ok` 改 `__atomic` | buffer 线程写、采集线程读 |
| `main.c` | 退出顺序：`dev_bridge_stop()` → `cam_feed_stop()` → `detector_stop()` → `mqtt_hub_stop()` → `health_shutdown()` → `log_close()` | 先停生产者再落盘，避免退出期间继续产生事件 |

编译部署：

```bash
# VM
cd ~/shared/XRY/Edge-AI-Gateway
make deploy

# 板端
killall lvgl_gateway
GATEWAY_LOG_LEVEL=debug /opt/edge-gateway/current/lvgl_gateway &
```

#### 功能回归

```bash
sleep 10
grep -E "CAM|IMU|DET|BRIDGE" /opt/edge-gateway/logs/gateway.log | tail -10
top -b -n 1 | head -6
```

预期：

- `[DET] worker started, queue depth=2`；
- `conv=15 fps` 左右，CPU 与改动前接近（略降或持平）；
- 静态无 `motion hit`，人走过有 `motion hit`；
- 无 `SENSOR ... STALE` 抖动（IMU 阈值已放宽到 1500ms）。

#### 优雅退出（重点）

```bash
killall lvgl_gateway
```

预期日志顺序（1 秒内完成，进程退出码 0）：

```
[MAIN] application exit
[BRIDGE] device bridge stopped
[CAM] camera feed stopped
[DET] worker stopped (hits=N dropped=M)
[MQTT] stopped, pending=N dropped=0
[LOG] closed (dropped=0)
```

检查：

```bash
ls /tmp/lvgl_gateway.lock 2>/dev/null || echo "lock released"
pidof lvgl_gateway || echo "process gone"
```

> 如果退出卡住，说明某个 join 被阻塞（优先看 IMU `read`、摄像头 `DQBUF`、
> 精判 `cloud_detect_query` 2.5s 超时），把 `wchan` 发出来定位。

#### 已知限制

- `detector_stop()` 在云端复核进行中时，`join` 最长等 HTTP 超时（2.5s）；
- 摄像头 `DQBUF` 在传感器异常时可能阻塞，`cam_feed_stop()` 会等待（正常 33ms 一帧）；
- 有界队列深度固定 2（编译期宏 `DET_QUEUE_DEPTH`），后续可做成配置项。

验收记录：

| 日期 | 项目 | 结果 | 备注 |
|---|---|---|---|
| 2026-11-10 | 代码完成 | 完成 | 帧锁/任务队列/原子计数/可 join 线程 |
| 2026-11-10 | 功能回归 | 通过 | `[DET] worker started, queue depth=2`；`conv=15 fps`、`conv_ms≈25`；无 STALE |
| 2026-11-10 | 优雅退出 | 通过 | `killall` 后 54ms 完成：IMU/BRIDGE/CAM/DET/MQTT 依次 stopped |
| 2026-11-10 | 资源释放 | 通过 | 锁文件已删除、进程退出、可立即重启 |
| 2026-11-10 | 内存/CPU | 通过 | VSZ 114m（与改造前一致），CPU ≈56% |

### 3.20 T13 动态帧率（代码完成，待板端验证）

**目标**：无人时降低摄像头转换/预览频率省 CPU，检测到运动后自动提高帧率。

实现（`cam_feed.c`）：

| 状态 | 转换/预览间隔 | 说明 |
|---|---|---|
| idle | `CAM_FPS_MS_IDLE = 200ms`（5fps） | 默认状态，单核下给 UI/检测让出 CPU |
| active | `CAM_FPS_MS_ACTIVE = 33ms`（30fps） | 运动命中后进入 |
| 保持 | `CAM_ACTIVE_HOLD_MS = 10000ms` | 最后一次命中后保持 10s，然后回落 idle |

- 运动命中时间戳 `s_last_hit_ms`（`CLOCK_MONOTONIC`，uint32 回绕安全）；
- 采集仍按 30fps 交付，仅降低**转换/预览**频率；
- 模式切换打印一条 `[CAM] fps mode -> idle(5fps)/active(30fps)`。

验证：

```bash
killall lvgl_gateway
GATEWAY_LOG_LEVEL=debug /opt/edge-gateway/current/lvgl_gateway &
sleep 10
grep -E "dynamic fps|fps mode|conv=" /opt/edge-gateway/logs/gateway.log | tail -6
```

预期：

```
[CAM] dynamic fps: idle=200ms(5fps) active=33ms(30fps) hold=10000ms
[CAM] fps mode -> idle(5fps)
[CAM] sensor=30 fps, conv=5 fps, conv_ms=...
```

人走过画面：

```
[CAM] motion hit area=...
[CAM] fps mode -> active(30fps)
[CAM] sensor=30 fps, conv=30 fps, conv_ms=...
```

停止运动约 10 秒后回到 `fps mode -> idle(5fps)`、`conv=5 fps`。

CPU 对比：

```bash
top -b -n 1 | head -6
```

预期 idle 时 `lvgl_gateway` 约 30~40%，active 时回到 ~56%。

注意：

- idle 5fps 下预览会明显变慢（省电取舍），需要跟手时可把 `CAM_FPS_MS_IDLE` 调到 100（10fps）；
- 运动检测在 5fps 下仍然有效，但极快的小幅动作可能漏检。

验收记录：

| 日期 | 项目 | 结果 | 备注 |
|---|---|---|---|
| 2026-11-11 | 动态帧率实现 | 完成 | idle 5fps / active 30fps / hold 10s |
| 2026-11-11 | idle 模式 | 通过 | `fps mode -> idle(5fps)`、`conv=5 fps` |
| 2026-11-11 | active 模式 | 通过 | 运动后 `fps mode -> active(30fps)`，停止约 10s 回 idle（多轮循环） |

### 3.21 C2 IMU 峰值/RMS + buffer 模式预警修复（代码完成，待验证）

**发现的问题**：IMU 预警判定原来只写在 `!g_imu_buffer_ok` 的 sysfs 回退分支里，
**buffer 模式下阈值预警实际不生效**（只有数据展示）。

**改动**（`dev_bridge.c`）：

1. 合加速度 `mag = sqrt(ax²+ay²+az²)×100` 在 buffer 与 sysfs 两条路径都计算并写入 `g_shm`；
2. 新增 32 点滑窗统计：瞬时 `mag`、`RMS`、`峰值`（buffer 100Hz≈0.32s，sysfs 10Hz≈3.2s）；
3. IMU 预警判定移到公共块（每 100ms 读 `g_shm`），**两条通路都生效**；
4. 失败时清 `imu_ok`，避免用陈旧值继续判警；
5. 每 5s 打印一条 DEBUG 统计：`[BRIDGE] imu mag=.. rms=.. peak=..`；
6. `struct dev_diag` 增加 `imu_mag_x100 / imu_rms_x100 / imu_peak_x100`。

验证：

```bash
GATEWAY_LOG_LEVEL=debug /opt/edge-gateway/current/lvgl_gateway &
sleep 10
grep "imu mag=" /opt/edge-gateway/logs/gateway.log | tail -3
```

实测（2026-11-11，2.8.0）：

```
静止: imu mag=52  lin=0  rms=0  peak=0
晃动: imu mag=137 lin=9x rms=8x peak=1xx
```

> `mag` 是**校准后**的合加速度（含角度残留），`lin` 是去掉重力后的线性加速度；
> `rms/peak` 统计的是 `lin`（静止≈0，峰值反映真实运动强度）。

预警联动（buffer 模式）：

1. 设置页把某分区改为 `ICM20608 / LIN（去重力）/ > 阈值`（默认 30，预警 15）；
2. 晃动设备 → 时间轴出现该分区预警/告警；
3. 静止 → 不再触发（`lin≈0`，不再受安装角度影响）。

验收记录：

| 日期 | 项目 | 结果 | 备注 |
|---|---|---|---|
| 2026-11-11 | 滑窗 RMS/峰值实现 | 完成 | 32 点，统计对象为 `lin`（去重力） |
| 2026-11-11 | 静止基线 | 通过 | `mag=52 lin=0 rms=0 peak=0` |
| 2026-11-11 | buffer 模式预警修复 | 代码完成 | 判定改为公共块，两通路生效；待阈值联动验证 |

### 3.22 C1/C5 多源融合 + 置信度评分（代码完成，待验证）

**目标**：摄像头与传感器在时间窗内互相印证时才升级告警，降低单源误报；
默认**关闭**，保持原有即时告警语义。

实现（`fusion.c/h` + `dev_bridge.c`）：

| 规则 | 值 |
|---|---|
| 事件窗口 | `fusion_window_ms`，默认 **3000ms** |
| 单源强度 | 摄像头/传感器各 55 |
| 单源封顶 | **60**（低于阈值，只做轻提醒） |
| 双源共现 | +25 → 80 |
| 告警阈值 | `fusion_threshold`，默认 **70** |
| 每分区事件缓存 | 8 条（来源/时间戳/强度） |

配置（`gateway.json` 的 `warn` 节，也可 `set_config` 下发）：

```json
"fusion_on": 0, "fusion_threshold": 70, "fusion_window_ms": 3000
```

远程开启：

```bash
mosquitto_pub -h 127.0.0.1 -u gateway -P '<密码>' \
  -t 'gateway/gw-<id>/cmd' \
  -m '{"cmd":"set_config","id":30,"fusion_on":1,"fusion_threshold":70,"fusion_window_ms":3000}'
```

验证：

1. **单源**：只晃动设备（摄像头无运动）→ 日志
   ```
   [FUSE] zone=0 sensor event queued
   [FUSE] zone=0 score=55 -> single-source
   ```
   时间轴出现“单源事件（未达融合阈值）”，**不告警**。

2. **双源**：人走过镜头的同时有传感器触发 → `score=80` →
   ```
   [FUSE] zone=0 score=80 -> ALARM
   ```
   正常告警。

3. 关闭融合（`fusion_on=0`）恢复即时告警。

注意：

- 开启后告警最多延迟一个时间窗（默认 3s）；
- 只有“布防中”的分区才参与定案；
- 单源事件会写入时间轴（low 级），便于回溯。

验收记录：

| 日期 | 项目 | 结果 | 备注 |
|---|---|---|---|
| 2026-11-11 | 融合/评分实现 | 完成 | 窗口/阈值/单源封顶/双源加分 |
| 2026-11-11 | 板端验证 | 待验证 | 需开启 `fusion_on` 后单源/双源各测一次 |

### 3.23 C3 IMU 姿态补偿（重力分离）（代码完成，待验证）

**问题**：静态基线 `imu_off` 是在某个固定姿态下采集的，设备换个角度后
合加速度会出现“残留”（例：静止 `mag=52`，即 0.52g），不能用绝对阈值判断运动。

**实现**（`dev_bridge.c`）：

- 对校准后的三轴做低通，估计重力向量 `g`（α=0.02，100Hz 下时间常数≈0.5s）；
- 线性加速度 `lin = |a - g|`，与摆放角度无关；
- `lin ×100` 写入 `g_shm.imu_lin_x100` / `struct dev_diag.imu_lin_x100`；
- 5s 统计日志增加 `lin`：`imu mag=.. lin=.. rms=.. peak=..`。

**温漂拟合**：已实现为**纯自动拟合**（见 3.27）——每次零偏采集记录一个温度点，
累计 ≥2 个不同温度点后自动拟合斜率并补偿；未拟合前不补偿。

验证：

```bash
GATEWAY_LOG_LEVEL=debug /opt/edge-gateway/current/lvgl_gateway &
sleep 8
grep "imu mag=" /opt/edge-gateway/logs/gateway.log | tail -3
```

预期：

```
静止: imu mag=52 lin=1  rms=2  peak=3      ← mag 有角度残留，lin≈0
晃动: imu mag=137 lin=9x rms=8x peak=1xx    ← lin 明显上升
改变摆放角度后再静止: lin 仍回 0 附近
```

验收记录：

| 日期 | 项目 | 结果 | 备注 |
|---|---|---|---|
| 2026-11-11 | 重力分离实现 | 完成 | α=0.02 低通，lin 入 diag |
| 2026-11-11 | 静止基线 | 通过 | `mag=52 lin=0`，角度残留被消除 |
| 2026-11-11 | 晃动/换角度 | 待验证 | 需观察 lin 上升与回零 |
| 2026-11-11 | 温漂自动拟合 | 完成 | 2 个温度点起拟合，见 3.27 |

### 3.24 C4 摄像头 ROI / 阴影抑制（代码完成，待验证）

**目标**：屏蔽固定误报区域（墙面/窗帘/窗外），并降低阴影造成的误报。
默认**关闭**，不影响已验证的运动检测参数。

实现（`cam_feed.c`）：

| 能力 | 开关 | 说明 |
|---|---|---|
| ROI | `GATEWAY_CAM_ROI="x0,y0,x1,y1"`（百分比 0~100） | 区域外前景清零；全幅=关闭 |
| 阴影抑制 | `GATEWAY_CAM_SHADOW=1` | 仅“轻微变暗”（差值 < 背景一半）的像素不计入运动 |

启动日志：

```
[CAM] motion roi=on [8,10,92,98] shadow_suppress=1
```

验证：

```bash
killall lvgl_gateway
GATEWAY_LOG_LEVEL=debug \
GATEWAY_CAM_ROI="20,20,80,80" GATEWAY_CAM_SHADOW=1 \
  /opt/edge-gateway/current/lvgl_gateway &
sleep 10
grep -E "motion roi|motion hit" /opt/edge-gateway/logs/gateway.log | tail -5
```

| 场景 | 预期 |
|---|---|
| 手在画面边缘（ROI 外）移动 | 无 `motion hit` |
| 手在 ROI 内移动 | 出现 `motion hit` |
| 开灯后物体投影变化 | 阴影抑制开启时不应持续命中；关闭时可能命中 |

注意：

- ROI 是按“降采样 80×50”坐标计算的，百分比四舍五入；
- 阴影抑制是启发式（只处理轻微变暗），强阴影或大面积遮挡仍可能命中；
- 两个开关都通过环境变量，不改配置文件；需要持久化可写进 rcS。
- **画面方向**：默认 `V=1, H=0`（上下翻、不左右翻）。若画面左右反了用
  `GATEWAY_CAM_HFLIP=1`；若上下也反了用 `GATEWAY_CAM_VFLIP=0`；两个都设 0 即不翻转。
  启动日志会打印 `flip v=.. h=..`，可据此确认。

验收记录：

| 日期 | 项目 | 结果 | 备注 |
|---|---|---|---|
| 2026-11-11 | ROI / 阴影抑制实现 | 完成 | 环境变量开关，默认关闭 |
| 2026-11-11 | 板端验证 | 待验证 | ROI 内外、阴影场景各测一次 |

### 3.25 D1 PC 端单元测试（已完成）

**范围**：纯逻辑模块，宿主机 gcc 编译，不需要板子。

```bash
cd ~/shared/XRY/Edge-AI-Gateway
make test            # 等价于 cd tests && make run
```

| 用例 | 模块 | 覆盖点 |
|---|---|---|
| `test_hmac` | `ota_hmac.c` | SHA-256 / HMAC 标准向量、文件流式校验 |
| `test_config` | `config_mgr.c` | 默认值、解析/落盘/重载、字符串字段、裁剪、损坏回退 |
| `test_event_store` | `event_store.c` | 追加/去重/删除/重载/权限 600 |
| `test_sensor_mgr` | `sensor_mgr.c` | 滤波、越界升级、恢复、`invalid_cnt` 清零 |
| `test_fusion` | `fusion.c` | 单源封顶、双源加分、窗口定案、清空 |
| `test_sensor_calib` | `sensor_calib.c` | 温漂最小二乘拟合、温差不足、补偿量 |

预期结尾：`ALL TESTS PASSED`。

不在范围：`cam_feed`（V4L2/LVGL）、`detector`（ncnn）、MQTT/OTA 网络路径——由板端联调覆盖。

验收记录：

| 日期 | 项目 | 结果 | 备注 |
|---|---|---|---|
| 2026-11-11 | 测试框架与用例 | 完成 | `tests/` + `make test` |
| 2026-11-11 | 全量运行 | 通过 | 5 个模块，`ALL TESTS PASSED` |
| 2026-11-11 | 新增温漂用例 | 通过 | `test_sensor_calib`（第 6 个模块），`ALL TESTS PASSED` |

### 3.26 C2 双阈值 + IMU 阈值切到 LIN（代码完成，待验证）

**目标**：
1. 每个分区支持“预警 / 告警”两级：中间区间只做轻提醒（时间轴 + toast），
   达到告警阈值才进入告警状态机、驱动声光；
2. IMU 判定值从“合加速度（含重力）”切换为“去重力线性加速度（lin）”，
   阈值不再受安装角度影响。

实现：

| 文件 | 改动 |
|---|---|
| `dev_bridge.h` | `zone_warn_cfg` 增加 `threshold_warn`；`IMU_DATA_MAG` 语义改为 `lin`，新增同义名 `IMU_DATA_LIN` |
| `dev_bridge.c` | 两套防抖计数（告警/预警）；预警只在“未达告警阈值”时判定；主线程消费 `warn_pending` → toast + 时间轴（low），带冷却 |
| `config_mgr.c` | DEFAULT zone 30/15、前门 PS 30/20；JSON 增加 `threshold_warn`；v1→v2 迁移把旧 `150` 重标为 `30/15`；保证 `threshold_warn ≤ threshold` |
| `mqtt_hub.c` | `set_warn` 支持 `threshold_warn` |
| `ui_home.c` | 设置页新增 `PRE` 滑条（0=OFF）；通道按钮 `MAG` → `LIN`；删除过期的 `(void)cam_to_zone` 占位 |

配置示例：

```json
{"src": 2, "channel": 0, "threshold": 30, "threshold_warn": 15, "cmp": 0, "enabled": 1}
```

远程设置：

```bash
mosquitto_pub -h 127.0.0.1 -u gateway -P '<密码>' \
  -t 'gateway/gw-b8ae1d010000/cmd' \
  -m '{"cmd":"set_warn","id":40,"zone":3,"src":2,"channel":0,"threshold":30,"threshold_warn":15}'
```

板端验证：

```bash
killall lvgl_gateway
GATEWAY_LOG_LEVEL=debug /opt/edge-gateway/current/lvgl_gateway &
sleep 10
grep -E "pre-warn|imu mag=" /opt/edge-gateway/logs/gateway.log | tail -5
```

| 场景 | 预期 |
|---|---|
| 静止 | `imu mag=52 lin=0 ...`，无预警/告警 |
| 轻晃（lin 介于 15~30） | `zone=3 pre-warn`，时间轴“仓库 传感器预警”，**不响蜂鸣器** |
| 明显晃动（lin > 30） | 正常告警（`alarm zone=3`） |
| 旧 gateway.json（v1，阈值 150） | 启动日志 `IMU threshold migrated to lin semantics (30/15)` |

验收记录：

| 日期 | 项目 | 结果 | 备注 |
|---|---|---|---|
| 2026-11-11 | 双阈值实现 | 完成 | 预警不触发告警/声光 |
| 2026-11-11 | LIN 语义切换 | 完成 | 阈值 30/15，旧配置自动迁移 |
| 2026-11-11 | 板端迁移 | 通过 | `zone=3 IMU threshold migrated to lin semantics (30/15)` |
| 2026-11-11 | 板端行为验证 | 待验证 | 轻晃/明显晃动各一次 |

### 3.27 C3 温漂自动拟合（代码完成，待验证）

**模型**：

```
ax' = (ax_raw - (T - T_ref) * k_x / 1000 - off_x) * scale_x / 1000
k: LSB/℃ ×1000（0=未拟合，不补偿）
T: 温度原始值（326.8 LSB/℃）
```

**自动拟合流程**：

1. 每次零偏采集（静止 2s）除了得到基线，还把 `(温度, 三轴均值)` 记为一个温度点；
2. 最多保留 4 个点；同一温度（±1℃）覆盖旧点，避免重复点挤掉有效温差；
3. 点数 ≥2 且温差 ≥2℃ 时，用最小二乘拟合每轴斜率 `k`，写入配置；
4. 已有斜率但新点温差不足时，把新基线折算回参考温度，避免重复补偿；
5. 未拟合（`k=0`）时补偿量为 0，行为与旧版一致。

持久化字段（`gateway.json` 的 `calib` 节）：

```json
{"temp_k": [k_x, k_y, k_z], "temp_ref": 9000, "pts": 2,
 "pt_temp": [7000, 9000, 0, 0],
 "pt_off_x": [...], "pt_off_y": [...], "pt_off_z": [...]}
```

日志：

```
[CALIB] static baseline done: off=[..] temp_avg=9000 samples=200 pts=2
[CALIB] temp drift fitted: k=[..] ref=9000
[CALIB] temp drift not updated (ret=-2, need >=2 points / 2C spread)
```

板端验证（需要让设备经历两个温度）：

```bash
# 第 1 次校准：常温静置
# 通过设置页/串口触发零偏采集，或重启（GATEWAY_CALIB_ONCE=1）后等待 2s
grep -E "CALIB" /opt/edge-gateway/logs/gateway.log | tail -5

# 用热风/空调让板子温差 ≥2℃ 后，再校准一次
grep -E "temp drift fitted|temp drift not" /opt/edge-gateway/logs/gateway.log | tail -3
```

| 场景 | 预期 |
|---|---|
| 首次校准 | `pts=1`，`temp drift not updated (ret=-2 ...)`，`k=0` |
| 温差 ≥2℃ 后第二次校准 | `pts=2`，`temp drift fitted: k=[...] ref=...` |
| 之后重启 | `[CALIB] temp drift: k=[...] ref=... pts=2`（已持久化） |
| 单温度反复校准 | 覆盖同一温度点，`pts` 不增长 |

PC 端单元测试：`tests/test_sensor_calib.c`（线性/带噪声/温差不足/补偿量）。

验收记录：

| 日期 | 项目 | 结果 | 备注 |
|---|---|---|---|
| 2026-11-11 | 拟合与持久化实现 | 完成 | ≥2 点且温差 ≥2℃ 才生效 |
| 2026-11-11 | 单元测试 | 通过 | `test_sensor_calib`：2000/-1000/4000、噪声 2100/4100、补偿 6536 |
| 2026-11-11 | 板端双温度验证 | 待验证 | 需制造 ≥2℃ 温差 |

### 3.28 T11 单向 TLS（mbedTLS + 自签 CA）

**范围**：MQTT 传输层加密（1883 → 8883），**单向认证**（板端校验服务器证书），
客户端仍用用户名/密码。mTLS（双向证书）未做。

实现：

| 文件 | 改动 |
|---|---|
| `mqtt_transport.c/h` | 新增传输层：TCP / mbedTLS 透明切换、超时、`readable` 兼容 TLS 缓冲 |
| `mqtt_hub.c` | 改用传输层；配置 `mqtt_port/mqtt_tls/mqtt_ca_path`；日志打印 `transport: TLS/plain` |
| `config_mgr.c/h` | 新增三个字段（默认 1883 / 0 / `/opt/edge-gateway/certs/ca.crt`） |
| `core/lvgl/Makefile` | 检测到 `third_party/mbedtls-armhf/lib/libmbedtls.a` 时自动 `-DMQTT_TLS` |
| `scripts/build_mbedtls.sh` | 交叉编译 mbedTLS 2.28 LTS（静态库） |
| `cloud/gen_tls_certs.sh` | 生成自签 CA + 服务器证书（SAN 含 broker IP） |
| `cloud/mosquitto.conf` | 增加 `listener 8883`（TLS 1.2，`require_certificate false`） |
| 顶层 `Makefile` | `make deploy` 复制 `ca.crt` 到板端 `/opt/edge-gateway/certs/` |

步骤（【开发机 VM】）：

```bash
cd ~/shared/XRY/Edge-AI-Gateway
./scripts/build_mbedtls.sh          # 需联网；产物 third_party/mbedtls-armhf
cd cloud && ./gen_tls_certs.sh      # 证书 SAN 默认 IP 192.168.3.26
cd ..
make lvgl 2>&1 | tail -5            # 自动带 -DMQTT_TLS
strings build/lvgl/lvgl_gateway | grep -c "TLS handshake ok"   # 期望 1
```

> **兼容性说明**：mbedTLS 2.28 的 `x509_crt_check_san()` 只匹配 `dNSName`，
> **不识别 `iPAddress` 类型的 SAN**。因此证书 SAN 里同时写入
> `DNS:<broker IP>`，板端用 IP 直连才能通过主机名校验（否则报 `-0x2700`）。
> OpenSSL 侧验证 IP 请用 `-verify_ip`（`-verify_hostname` 对 IP 会误报 mismatch）。

启用 TLS（二选一）：

```json
// gateway.json 顶层
"mqtt_port": 8883, "mqtt_tls": 1,
"mqtt_ca_path": "/opt/edge-gateway/certs/ca.crt"
```

```bash
# 或环境变量（临时）
GATEWAY_MQTT_PORT=8883 GATEWAY_MQTT_TLS=1 \
GATEWAY_MQTT_CA=/opt/edge-gateway/certs/ca.crt \
  /opt/edge-gateway/current/lvgl_gateway &
```

验证：

```bash
# 【开发机 VM】broker 侧自检
cd ~/shared/XRY/Edge-AI-Gateway/cloud
./cloudctl.sh restart
mosquitto_sub -h 127.0.0.1 -p 8883 --cafile certs/ca.crt \
  -u gateway -P '<密码>' -t 'gateway/#' -v -C 1 -W 3
```

```bash
# 【板端】
grep -E "transport:|TLS handshake|connected .*TLS" /opt/edge-gateway/logs/gateway.log | tail -5
```

预期：

```
[MQTT] transport: TLS port=8883 ca=/opt/edge-gateway/certs/ca.crt
[MQTT] TLS handshake ok (sni=192.168.3.26, ca=/opt/edge-gateway/certs/ca.crt)
[MQTT] connected 192.168.3.26:8883 (TLS), subscribe+online
```

负向验证：

| 场景 | 预期 |
|---|---|
| 板端 CA 换成别的证书 | `TLS: handshake failed -0x2700`（证书校验失败），不进 MQTT |
| broker 8883 未开 | 连接失败 + 指数退避重试，不崩溃 |
| `mqtt_tls=0` 仍连 1883 | 与旧版行为一致（向后兼容） |

验收记录：

| 日期 | 项目 | 结果 | 备注 |
|---|---|---|---|
| 2026-11-11 | 传输层/TLS 代码 | 完成 | 未编 mbedTLS 时自动回退明文 |
| 2026-11-11 | 证书/ broker 8883 | 完成 | 自签 CA，SAN=broker IP + DNS 别名 |
| 2026-11-11 | 板端握手 | 通过 | `TLS handshake ok` → `connected ...:8883 (TLS)` |
| 2026-11-11 | 兼容性坑 | 记录 | mbedTLS 2.28 不识别 iPAddress SAN，需 `DNS:<IP>` |

### 3.29 T12 精简版：命令 ACK + 超时重试

**范围**：只做“命令可靠下发”，不含设备注册/心跳/区域联防。

实现：

| 位置 | 改动 |
|---|---|
| 设备端 `mqtt_hub.c` | ACK 已有；新增 **命令 id 去重（16 条）**，重试只补 ACK 不重复执行（保护 OTA） |
| 云端 `cloud/cmd_send.py` | paho-mqtt：发布 → 等同 id ACK → 超时重发（默认 2s×3） |
| 云端 `cloud/server.py` | `POST /cmd`（走 X-API-Key），线程池执行，不阻塞事件循环 |
| `cloud/requirements.txt` | 增加 `paho-mqtt` |

安装依赖（【开发机 VM】，cloudctl 优先用 `~/cloud-venv`）：

```bash
python3 -m venv ~/cloud-venv 2>/dev/null || true
~/cloud-venv/bin/pip install -r cloud/requirements.txt
echo 'Gw#2026pass' > cloud/secrets/mqtt.pass && chmod 600 cloud/secrets/mqtt.pass
```

用法：

```bash
# 直接下发（CLI，用装了 paho 的 venv）
cd ~/shared/XRY/Edge-AI-Gateway/cloud
~/cloud-venv/bin/python3 ./cmd_send.py --device gw-b8ae1d010000 --cmd set_config \
  --params '{"brightness":70}' --timeout 2 --retries 3

# 或走 HTTP（需要 X-API-Key）
curl -s -X POST http://127.0.0.1:8000/cmd \
  -H "X-API-Key: $(cat secrets/api.key)" -H 'Content-Type: application/json' \
  -d '{"device":"gw-b8ae1d010000","cmd":"set_config","params":{"brightness":70},"timeout":2,"retries":3}'
```

预期返回：

```json
{"ok": true, "attempts": 1, "ack": {"id": 1730000000001, "cmd": "set_config", "ok": true}, "id": 1730000000001}
```

重试与幂等验证：

| 场景 | 操作 | 预期 |
|---|---|---|
| 正常 | 板端在线，`set_config` | `attempts=1`，配置生效 |
| ACK 丢失 | 先 `killall lvgl_gateway` 再下发 | `attempts=4`，`ok=false`（不阻塞其他请求） |
| 重试不重复执行 | 用同一 `--id` 连发两次 | 板端第二次日志 `duplicate -> ack only`，OTA 不会被重复触发 |
| 鉴权 | 不带 X-API-Key 调 `/cmd` | 401 |

验收记录：

| 日期 | 项目 | 结果 | 备注 |
|---|---|---|---|
| 2026-11-11 | 设备端 id 去重 | 完成 | 最近 16 个 id |
| 2026-11-11 | 云端 ACK/重试 | 完成 | CLI + `POST /cmd` |
| 2026-11-11 | 板端联调 | 通过 | 正常 `attempts=1`；断线 `attempts=4, ok=false`；同 id 第二次 `duplicate -> ack only` |

### 3.30 单摄语义修正：去掉“四路”假象

**背景**：单个固定安装的摄像头只能看到一个视场，之前 UI 的 4 个通道切换
只是把同一画面贴四个标签，告警归属（按“当前通道”）也随之失真。

**改动**：

| 位置 | 改动 |
|---|---|
| `ui_home.c` | 删除 4 通道 pill / `cam_to_zone` / `g_current_cam`；摄像头标题固定 `CAM · Nfps`；预览文字显示归属防区 |
| 配置 | 新增 `camera_zone`（0~3，默认 0），表示摄像头的固定归属防区 |
| `ui_events_current_cam_zone()` | 改为返回 `config.camera_zone`，不再跟随通道切换 |
| 抓拍/时间轴 | 位置使用防区名（如“仓库”），不再用 `CH0x` |
| 镜像 | 默认 `V=1,H=0`；`GATEWAY_CAM_VFLIP/HFLIP` 可覆盖 |

**语义**：

- 摄像头 = 1 路固定画面，服务 1 个防区；
- 四个分区卡片 = 四个**逻辑防区**的状态（布防/告警），不是四路视频；
- 摄像头告警统一归属 `camera_zone`。

验证：

```bash
# 把摄像头防区改为仓库（3）
cd ~/shared/XRY/Edge-AI-Gateway/cloud
~/cloud-venv/bin/python3 ./cmd_send.py --device gw-b8ae1d010000 \
  --cmd set_config --params '{"camera_zone":3}'
```

| 场景 | 预期 |
|---|---|
| UI 摄像头区 | 显示 `CAM · 30fps`、`单摄预览 · 防区：仓库`，无 4 个切换按钮 |
| 人走过镜头 | 告警归属“仓库”，时间轴位置为“仓库” |
| 抓拍 | 相册/事件位置为“仓库” |
| 镜像 | 画面左右方向与实际场景一致 |

验收记录：

| 日期 | 项目 | 结果 | 备注 |
|---|---|---|---|
| 2026-11-11 | 去掉通道切换 | 完成 | UI 文案与归属逻辑同步 |
| 2026-11-11 | 镜像默认值 | 通过 | `flip v=1 h=0`，画面方向正常 |
| 2026-11-11 | 板端验证 | 通过 | `camera_zone=3` 后文案与告警归属均为“仓库” |

### 3.31 PWM 蜂鸣器对摄像头的干扰（硬件耦合）

**现象**：点击“调速”（PWM3 无源蜂鸣器）后，摄像头画面出现行错位；**抓拍照片本身也错位**，
GPIO 灯/蜂鸣不影响。

**结论**：不是显示撕裂，也不是 CSI 信号错误——是 PWM 蜂鸣器的电流脉冲/机械振动
影响了 OV5640 采集。

| 验证 | 结果 |
|---|---|
| 抓拍照片 | 同样错位 → 采集侧受扰 |
| 仅 GPIO 灯 / GPIO 蜂鸣 | 正常 |
| 仅 PWM 调速 | 错位 |
| `dmesg` | 无 CSI/I2C 报错（更像供电/振动） |

**软件缓解**：手动调速占空比由 50% 降为 **30%**（`dev_bridge_set_pwm`）。

**硬件建议**（按效果排序）：

1. 蜂鸣器单独 5V 供电 + 三极管驱动，地线与摄像头地单点汇合；
2. 蜂鸣器电源脚并 100~470µF 电解 + 0.1µF 瓷片；
3. 蜂鸣器远离摄像头 FPC，或加海绵/泡棉减振；
4. 若是电磁式无源蜂鸣器，反向并续流二极管。

**进一步定位**：拔掉蜂鸣器后点“调速”——

- 不再错位 → 机械振动/声音耦合；
- 仍错位 → PWM 引脚/电源布线耦合。

验收记录：

| 日期 | 项目 | 结果 | 备注 |
|---|---|---|---|
| 2026-11-11 | 现象定位 | 完成 | 抓拍照片也错位 → 采集侧 |
| 2026-11-11 | 占空比 50%→30% | 待验证 | 观察是否仍错位 |
| 2026-11-11 | 硬件整改 | 待做 | 独立供电/电容/减振 |

### 3.32 应用内重启按钮（摄像头卡死恢复）

**入口**：设置 → 维护 → **重启应用**。

**实现**：

1. UI 调 `health_request_restart()` 置标志；
2. 主循环检测到标志 → `g_quit=1`，走现有优雅退出（停采集/相机/检测/MQTT，落盘事件）；
3. `health_shutdown()` **magic close 看门狗 + 删除单实例锁**；
4. `execl("/proc/self/exe", ...)` 原地重启（PID 不变，环境变量保留）。

**为什么不用 `killall` 后重启**：没有守护进程，杀掉后没人拉起；按钮走 exec 可以自恢复。

验证：

```bash
# 板端触发后看日志
grep -E "restart requested|application start|single instance lock" \
  /opt/edge-gateway/logs/gateway.log | tail -6
```

预期顺序：`restart requested by UI` → 一批 `stopped` → `application start` → `single instance lock (pid=同一 PID)`。

验收记录：

| 日期 | 项目 | 结果 | 备注 |
|---|---|---|---|
| 2026-11-11 | 重启接口 + 按钮 | 完成 | exec 自身，释放锁/看门狗 |
| 2026-11-11 | 板端验证 | 待验证 | 摄像头卡死场景下点击恢复 |

### 3.33 本地调试终端（检测 → 终端）

**入口**：检测页第 6 张卡 **终端（TTY）**。

**界面**：

| 区域 | 内容 |
|---|---|
| 输出区 | 黑底绿字，可滚动，保留最近 4KB |
| 命令行 | 单行输入框 + 执行 / 键盘 / 清屏 |
| 快捷命令 | `uname -a` / `free -m` / `ps` / `dmesg \| tail -15` / `ifconfig` |
| 软键盘 | 点“键盘”弹出 LVGL 键盘，OK 执行并收起，X 收起 |

**实现**：

- `fork + /bin/sh -c`，stdout/stderr 重定向到管道；
- 管道设 `O_NONBLOCK`，LVGL 定时器 100ms 读取并追加到 label，**不阻塞 UI**；
- 新命令会终止上一条未结束命令；
- 离开子页 / 切换卡片时终止子进程并关闭管道；
- 键盘为 `LV_KEYBOARD_MODE_TEXT_LOWER`，绑定单行 `lv_textarea`。

**安全说明**：该终端以应用权限（root）执行本地命令，仅用于现场调试；
演示/交付时可在设置中隐藏入口或加口令。

验证：

1. 进入终端 → 输入 `uname -a` → 点 OK/执行，输出出现内核版本；
2. 点“键盘” → 屏幕下方弹出键盘，输入 `free -m` → OK 执行并收起；
3. 点快捷 `dmesg` → 输出最近 15 行；
4. 返回再进入 → 上一条命令被终止，界面正常。

验收记录：

| 日期 | 项目 | 结果 | 备注 |
|---|---|---|---|
| 2026-11-12 | 终端 UI + 异步执行 | 完成 | fork/pipe/非阻塞 |
| 2026-11-12 | 软键盘 | 完成 | TEXT_LOWER，OK/CANCEL |
| 2026-11-12 | 板端验证 | 待验证 | 触屏输入与输出滚动 |

## 4. 后续阶段验证占位

| 阶段 | 内容 | 状态 | 验证入口 |
|---|---|---|---|
| T3-B | ICM20608 IIO buffer + trigger（100~200Hz） | 已完成 | 见 3.11 |
| T3-C | 零偏/六面/温补校准 + `calib.json` | 已完成（静态基线） | 见 3.9 |
| T4 | 摄像头误报抑制 | 已完成 | 见 3.12 |
| T5 | 线程模型/同步重构 | 已完成（54ms 优雅退出） | 见 3.19 |
| T6 | 断网缓存与补传 | 已完成 | 见 3.13 |
| T7 | MQTT 重连/设备身份 | 已完成（在线/LWT/重连） | 见 3.17 |
| T8 | 看门狗/健康检查 | 已完成 | 见 3.14 |
| T9 | 应用 OTA | 已完成（HTTP 拉取 + 重启确认 + 回滚） | 见 3.15 |
| T10 | 远程配置 | 已完成（命令/ACK/落盘） | 见 3.17 |

> 每完成一项，在对应章节补充：编译命令、启动步骤、正常输出、故障注入、验收记录。

---

## 5. 通用排障

| 现象 | 排查 |
|---|---|
| `没有规则可制作目标 AI` | 项目路径含空格；`pwd` 确认无空格 |
| `make: *** No rule to make target` | 先 `make clean`，再 `make lvgl` |
| 配置不落盘 | 检查日志 `[CONFIG] use path`；确认目录可写；rootfs 只读时走回退 |
| 日志文件不生成 | 检查 `[LOG] file=`；确认 `/opt/edge-gateway/logs` 可写 |
| 日志无 DEBUG | `GATEWAY_LOG_LEVEL=debug` 未设置，或启动前已设为 INFO |
| 配置/日志路径混乱 | 用 `GATEWAY_CONFIG`、`GATEWAY_LOG_DIR` 显式指定 |
| 启动黑屏 | 参考 README 故障排查（fb blank / 排线） |

---

## 6. 快速命令速查

```bash
# 编译
cd ~/shared/XRY/Edge-AI-Gateway
make clean && make lvgl

# 启动（板端）
/usr/bin/lvgl_gateway &

# 看配置
cat /opt/edge-gateway/config/gateway.json

# 看日志
tail -f /opt/edge-gateway/logs/gateway.log

# 调试级别
GATEWAY_LOG_LEVEL=debug /usr/bin/lvgl_gateway &

# 停止
killall -TERM lvgl_gateway
```
