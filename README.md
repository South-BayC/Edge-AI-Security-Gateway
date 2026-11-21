# Edge AI Security Gateway — 边缘智能安防网关

基于正点原子 I.MX6ULL ALPHA（EMMC 版）+ 7 寸 1024x600 触摸屏的嵌入式 Linux 综合工程，包含两个子项目：

- **项目 A · 边缘智能安防网关**（`core/lvgl/` 应用 + `cloud/` 云端服务）
  LVGL 中文触摸界面；帧差粗判 → 本地 SCRFD 人脸初判 → 云端复核定案（人员/类型判定 + 白名单比对）的两级入侵检测管线；
  告警分级声光、抓拍存证、事件时间轴、MQTT 上报、OTA 升级。
- **项目 B · 全外设驱动工程**（`core/` 各驱动模块）
  LED / 按键中断 / 多通道事件采集（kfifo+tracepoint）/ PWM 蜂鸣 / I2C 光感 / SPI 六轴 IMU / input 按键 / GT9147 触摸。

| 项 | 内容 |
|---|---|
| 硬件 | i.MX6ULL（单核 Cortex-A7 528MHz）· 7 寸 RGB 屏（mxsfb）· GT9147 触摸 · OV5640 CSI 摄像头 · TF 卡 · 以太网 |
| 系统 | Linux 4.1.15（Buildroot rootfs，NFS 开发模式） |
| 工具链 | `arm-linux-gnueabihf-`（4.9.4，含 g++） |
| 开发机 | Ubuntu VM（NFS rootfs + TFTP + 云端服务同机） |
| 代码规模 | 应用层手写 C/C++ 约 1.9 万行 / 100 个文件（不含 LVGL 与生成字库） |

---

## 一、架构分层

应用层按四层组织，依赖单向（`app → ui → domain → infra`），跨层只通过 `event_bus`、配置结构体与显式回调通信。

```
core/lvgl/src/
├─ main.c                  # 组装根：port 初始化 → UI → 桥接 → 主循环
├─ app/
│  └─ mqtt_commands.c/h    #   云端命令分发（配置/OTA/回读 + ACK + id 去重）
├─ ui/                     #   只依赖 LVGL + domain 接口，不碰 socket/硬件驱动
│  ├─ ui_home.c            #     主页布局组装、定时器编排、事件分发、hooks 接线
│  ├─ ui_events.c/h        #     真实信号接入（状态/告警/日志/网络/配置刷新）
│  ├─ ui_internal.h        #     ui 层内部共享对象与刷新能力（仅 ui/ 可包含）
│  ├─ ui_theme.h           #     色板 + 字体
│  ├─ ui_widgets.c/h       #     通用控件（卡片/按钮/模态/toast/迷你按钮）
│  ├─ ui_terminal.c/h      #     调试终端（键盘输入直接回显，无输入框）
│  ├─ ui_canvas.c/h        #     canvas 像素绘制（入侵框叠加）
│  ├─ ui_time.c/h          #     事件时间字符串（HH:MM）
│  ├─ panels/
│  │  ├─ ui_status_bar.c/h #     顶部状态栏（标题/时钟/联网/布防）
│  │  ├─ ui_timeline.c/h   #     事件时间轴（计数/清空/条目/查看回调）
│  │  └─ ui_sensor.c/h     #     检测页（6 卡片 + 子页/校准/曲线/系统信息/终端）
│  └─ modals/
│     ├─ ui_alarm.c/h      #     告警确认弹窗（长按 1s 消警 + MUTE，hooks 注入）
│     ├─ ui_settings.c/h   #     设置弹窗（亮度/复核/阈值/定时/系统/关于）
│     ├─ ui_detail.c/h     #     分区详情（布防 ⇄ 撤防）
│     └─ ui_album.c/h      #     抓拍相册 + 查看器（含 TF 恢复）
├─ domain/                 #   纯粹业务逻辑，无 LVGL / socket / UI 依赖，可物理机单测
│  ├─ zone_state.c/h       #     分区状态唯一源（ONLINE/ARMED/ALARM/OFFLINE）
│  ├─ sensors/imu_math.c/h #     去重力 + 滑窗 RMS/峰值
│  ├─ alarm/warn_policy.c/h#     分级声光节拍、静音、手动锁定
│  ├─ warn_rule.c/h        #     双阈值 + 防抖判定（AP/IMU 共用）
│  └─ json_util.c/h        #     极简 JSON 取值/转义
├─ infra/                  #   设备与网络访问，无 UI 依赖
│  ├─ hal_io.c/h           #     LED / 蜂鸣器 / PWM 设备
│  ├─ sensor_io.c/h        #     IIO sysfs / ICM buffer / KEY0 input
│  ├─ sensor_pipeline.c/h  #     采集线程 + 校准 + 滤波 + 双阈值 + 诊断快照
│  ├─ mqtt_client.c/h      #     MQTT 3.1.1 报文与连接
│  └─ net_probe.c/h        #     TCP 可达性探测（UI 联网状态）
│
├─ dev_bridge.c/h          #   编排层：告警触发/消警、两级管线分发、融合定案、冷却、事件发布
├─ cam_feed.c/h            #   V4L2 采集线程（OV5640 640x480 YUYV）→ 630x340 RGB565，
│                          #   NEON 查表转换 → 画布双缓冲发布；帧差粗判引擎
├─ detector.cpp/h          #   精判工作线程（nice+10）：SCRFD 人脸初判 → 云端复核
├─ cloud_detect.c/h        #   云端复核客户端（HTTP POST 帧上传）
├─ mqtt_hub.c/h            #   MQTT 事件上报（常驻线程 + 重连退避 + 离线队列补传）
├─ mqtt_transport.c/h      #   TCP / mbedTLS 传输抽象
├─ event_bus.c/h           #   领域事件总线（有界队列 + 优先级）
├─ event_store.c/h         #   断网事件本地缓存
├─ alarm_fsm.c/h           #   告警状态机
├─ fusion.c/h              #   多源融合评分
├─ sensor_mgr.c/h          #   滤波 / 故障状态
├─ sensor_calib.c/h        #   零偏 / 温漂拟合
├─ storage_mgr.c/h         #   TF 抓拍持久化（BMP，满卡清理，重启回填）
├─ config_mgr.c/h          #   配置持久化（JSON，原子落盘）
├─ config_types.h          #   中立配置类型（zone_warn_cfg / dev_config）
├─ zone_types.h            #   中立状态枚举（zone_state_t）
├─ log_svc.c/h             #   分级日志（单实例、轮转、权限 600）
├─ health_mgr.c/h          #   单实例 / 看门狗 / 重启
├─ error_code.c/h          #   统一错误码
├─ http_get.c/h            #   HTTP 客户端（OTA 下载）
├─ ota_mgr.c/h             #   OTA 状态机（远程触发、HMAC 校验、安装/回滚）
├─ ota_hmac.c/h            #   HMAC-SHA256 流式校验
├─ time_sync.c/h           #   SNTP 时间同步
├─ thread_util.c/h         #   线程封装（栈大小、命名、join）
├─ lvgl_port.c/h           #   LVGL 适配（fbdev + 触摸自动探测 + VSYNC 状态）
├─ font/                   #   中文字库：SHSC 16/20px + Noto 补充字库（16px）
└─ sim_main.c              #   PC 模拟器入口（SDL，板端不编译）
```

依赖审计（`docs/architecture.md` §6 有可复现命令）：

```bash
# domain 不得依赖 LVGL / UI / socket
grep -rnE '#include .*(lvgl|ui/|sys/socket|netinet|arpa)' core/lvgl/src/domain
# infra 不得依赖 UI / LVGL
grep -rnE '#include .*(ui/|ui_home|lvgl)' core/lvgl/src/infra
# ui 不得直接操作 socket / 硬件驱动
grep -rnE '#include .*(sys/socket|netinet|arpa|linux/input|sys/ioctl|drivers/)' core/lvgl/src/ui
```

当前三条均为 clean。

---

## 二、入侵检测两级管线

```
[传感器] KEY0/AP3216C/ICM20608 ──IIO/input──┐
[摄像头] OV5640 CSI ──V4L2 640x480 YUYV────┤
                                           ▼
                            ┌──────────────────────────┐
                            │  infra/sensor_pipeline   │  采集 + 校准 + 滤波
                            │  dev_bridge（编排）      │  双阈值 + 冷却节流
                            └───────────┬──────────────┘
                                        │ 帧差粗判命中
                                        ▼
                            ┌──────────────────────────┐
                            │ detector 工作线程（两级）│
                            │ 段1 SCRFD 人脸初判(本地) │──► ncnn（/root/face.*）
                            │ 段2 云端复核（可开关）   │──► cloud/server.py（VM:8000）
                            └───────────┬──────────────┘
                                        ▼ 结论（初步/最终 × 四类）
                            ┌──────────────────────────┐
                            │ dev_bridge 结论分发      │──► 告警弹窗 + 分级声光 + 自动消警
                            │ （布防/告警状态门槛）    │──► 时间轴 / MQTT / 自动抓拍
                            └──────────────────────────┘      └► storage_mgr（TF 卡 BMP）
```

1. **帧差粗判**（`cam_feed.c`）：每 N 帧对比一次灰度差（间隔 1~30 帧、差值 5~100，设置页可调）→ 命中且布防中且过冷却（默认 60s）→ 抓最新帧提交精判。
2. **段1 · 本地初判**（SCRFD，ncnn，约 400~800ms）：检出人脸 → 立即 STRANGER 告警（不等云端）；未检出 → toast「画面变动」轻提醒，等云端定论。
3. **段2 · 云端复核**（设置页「云端复核」开启时才做）：云端 YOLOv8 全类别检测 + 白名单比对 → 最终定案。

| 云端结论 | 设备行为 |
|---|---|
| 白名单命中（已授权） | 在告警 → 自动消警（声光停 + 弹窗关 + 时间轴「告警解除」） |
| 陌生人（有人未录入） | 不在告警 → 升级 STRANGER 告警（兜住本地漏检的背身人） |
| 动物 / 物体 / 无人 | 在告警 → 自动消警（压掉本地误报）；不在告警 → 时间轴记录类型 |
| 云端不可达 | 初判未检出人 → INTRUDER 告警（不漏报）；已告警 → 维持本地结论 |

> 设计核心：**本地管快**（约 1s 初判响应）、**云端管准**（人形 + 类型 + 白名单纠偏）、**断网管稳**（本地结论兜底）。
> 关闭「云端复核」= 纯本地语义（未检出人脸即 INTRUDER）。

---

## 三、构建与部署

### 3.1 环境准备

```bash
# 内核源码树（驱动编译 KDIR）
/home/szh/linux/kernel/linux-imx-rel_imx_4.1.15_2.1.0_ga_southbay
# NFS rootfs / TFTP
/home/szh/linux/nfs/rootfs    /home/szh/linux/tftp
# 交叉编译链
arm-linux-gnueabihf-          （4.9.4，含 g++；ncnn 依赖）
```

部署路径用 `config.mk` 覆盖（不入库）：

```bash
cp config.mk.example config.mk    # 按实际修改 NFS_ROOT / TFTP_DIR
```

### 3.2 一键构建

```bash
make              # 全编：core 驱动 + dts + lvgl 网关应用
make core         # 只编译驱动模块
make dts          # 只编译设备树
make lvgl         # 只编译 lvgl 网关应用
make test         # PC 端单元测试（10 个用例，需 gcc）
make clean        # 清理 build/
make clean-tests  # 清理单测二进制

make deploy       # 构建并部署到开发板（需 sudo）：
                  #   core/dts/lvgl 全量构建
                  #   驱动 .ko → NFS /lib/modules/4.1.15/
                  #   dtb → TFTP 目录
                  #   lvgl_gateway → NFS /usr/bin/ 与 current 指向的 release
                  #   models/face.param|bin → NFS /root/
                  #   cloud/secrets/ota.key、cloud/certs/ca.crt（存在才复制）
```

> 注意：Makefile 的 `.o` 规则只跟踪 `lv_conf.h`；**修改 `.h` 头文件后需 `touch` 对应 `.c` 或清缓存重编**。

### 3.3 内核配置要点（重编内核不得丢失）

`VIDEO_MXC_CSI_CAMERA`（6ULL CSI 主机 mx6s_capture；**VIDEO_MXC_CAPTURE 是 IPU 栈对 6ULL 无效**）、`MXC_CAMERA_OV5640`、`SPI_GPIO + GPIO_74X164`（74HC595，ov5640 pwdn/rst 提供者）、IIO。
详见 `docs/kernel-patches-20260829.md`（含 mx6s_capture FCC、ov5640 PLL 30fps 补丁）与 `docs/architecture.md`。

### 3.4 云端精判服务（VM）

```bash
cd cloud
./gen_api_key.sh                      # 生成 secrets/api.key
python3 -m venv ~/cloud-venv && source ~/cloud-venv/bin/activate
pip install -r requirements.lock.txt  # 固定版本（requirements.txt 为宽松版）
# Python 3.12：face_recognition 依赖 pkg_resources，需 setuptools<81
pip install "setuptools<81"

# 白名单：授权人员露脸照放入 cloud/whitelist/<姓名>.jpg（文件名=姓名）
./cloudctl.sh start                   # 或 uvicorn server:app --host 0.0.0.0 --port 8000
wget -qO- http://127.0.0.1:8000/whitelist   # → {"names":["SZH"]}
```

- 白名单接口：`GET /whitelist`（查看）、`POST /whitelist/add?name=姓名`（body=照片）
- 白名单目录增删改**自动重载**，无需重启
- 比对容差 `FACE_TOLERANCE=0.6`（越小越严）
- 板端服务地址：`core/lvgl/src/cloud_detect.c` 的 `CLOUD_DETECT_SERVER`（默认 `192.168.3.26:8000`）
- 密钥统一放在 `cloud/secrets/`（不入库，由 `gen_*.sh` 生成）
- 协议与故障处理详见 `cloud/README.md`

---

## 四、使用指南（板端 UI）

**上电自启**（rcS → `/usr/bin/lvgl_gateway`）。主界面布局：
顶栏（时间/联网/布防状态）| 左侧摄像头预览（630x340，30fps）+ 单分区卡 | 右侧事件时间轴 + 传感器迷你面板 | 底栏大按钮。

### 4.1 日常操作

| 操作 | 方式 |
|---|---|
| 一键布防 / 撤防 | 底栏大按钮；分区卡绿光呼吸 = 布防中 |
| 分区独立布防 | 点分区卡 → 详情弹窗 → 状态按钮（布防 ⇄ 撤防） |
| 告警确认（消警） | 弹窗上**长按 1 秒**（进度条防误触）；MUTE = 临时静音，消警后自动解除 |
| 手动抓拍 | 预览区「抓拍」→ 存 TF 卡（BMP）→ 相册回看 |
| 事件时间轴 | 右侧列表（类型着色），「清空」一键清空；条目「览」打开相册 |
| 息屏与唤醒 | 10 分钟无操作自动息屏；**触摸或 KEY0 唤醒** |
| 背光亮度 | 设置弹窗滑条（联动 pwm-backlight） |

### 4.2 检测页（底栏「检测」）

6 张卡片：AP3216C / ICM20608 / 摄像头 / IO / 系统 / 终端。点击进入子页：

| 子页 | 内容 |
|---|---|
| AP3216C | 光照/接近/红外实时值 + 曲线（双 Y 轴）；本底校准、恢复默认、采样开关、ALS 比例 |
| ICM20608 | 三轴/温度/合加速度/线性加速度/滑窗 RMS·峰值 + 双曲线；零偏校准（2s）、清除、采样开关 |
| 摄像头 | 帧率曲线；**运动粗判**开关（帧差判定灵敏度）、对比间隔 N、灰度差阈值 |
| IO | LED / 蜂鸣 / 调速（PWM 频率滑条 + 30% 占空比）、按键电平 |
| 系统 | CPU / 内存 / 运行时长 + 日志尾部滚动 |
| 终端 | 调试 shell：无输入框，键盘输入直接回显提示行，回车执行；快捷命令（uname/free/ps/dmesg/ifconfig） |

### 4.3 设置弹窗

- **识别设置**：`云端复核` 开关（开 = 两级管线，默认；关 = 纯本地）
- **检测设置**：帧差间隔（1~30 帧）/ 灰度差阈值（5~100）
- **预警设置**：四位置 × 触发源 × 数据通道 × 告警阈值 × 预警阈值 × 比较方式 × 冷却时间
- **定时布防**：时间段自动布防（支持跨天，如 22:00–06:00）
- **系统 / 关于**：亮度、网络信息、版本、重启应用

### 4.4 告警分级（设计 2.2）

| 级别 | 分区 | 行为 |
|---|---|---|
| 高 | 窗户 / 周界 | LED 快闪 + 持续长鸣 |
| 中 | 仓库 | LED 中闪 + 鸣 400ms / 停 400ms |
| 低 | 门 | LED 慢闪 + 鸣 200ms / 停 600ms |

消警后自动解除静音。

### 4.5 验收场景（云端复核开 + 布防）

| # | 操作 | 预期 | 串口标志 |
|---|---|---|---|
| 1 | 白名单内的人露脸挥手 | STRANGER 立即告警 → 1~2s 后**自动消警** | `faces=1` → `cloud=known (person)` |
| 2 | 未录入的人露脸挥手 | 告警维持 | `cloud=stranger (person)` |
| 3 | 画面无人时开关灯触发 | 不告警，时间轴「云端复核：判定无人员」 | `faces=0` → `cloud=none` |
| 4 | `ifconfig eth0 down` 断网触发 | 无人脸→INTRUDER；有人脸→维持 | `cloud=unreachable` |
| 5 | 关「云端复核」再挥手 | 纯本地：检出脸 STRANGER / 无人脸 INTRUDER | 无 cloud 行 |
| 6 | 宠物 / 移动物入镜 | 不告警，时间轴「判定为动物活动」 | `cloud=none` |

---

## 五、单元测试

纯逻辑模块在开发机（PC）上跑，不依赖 LVGL/V4L2/ncnn：

```bash
cd tests
make run          # 编译并依次运行全部用例
make clean        # 清理产物
# 预期最后一行：ALL TESTS PASSED
```

| 用例 | 被测模块 | 断言数 |
|---|---|---|
| `test_hmac` | `ota_hmac.c` | 3（SHA-256/HMAC 标准向量 + 流式校验） |
| `test_config` | `config_mgr.c` | 48（默认值/JSON/落盘/重载/裁剪/损坏回退） |
| `test_event_store` | `event_store.c` | 15（追加/去重/删除/落盘/权限） |
| `test_sensor_mgr` | `sensor_mgr.c` | 6（滤波/越界升级故障/恢复） |
| `test_fusion` | `fusion.c` | 9（单源/双源评分/窗口过期/清空） |
| `test_sensor_calib` | `sensor_calib.c` | 13（温漂最小二乘/温差不足/补偿量） |
| `test_imu_math` | `domain/sensors/imu_math.c` | 7（重力分离收敛/冲击/滑窗 RMS·峰值） |
| `test_warn_policy` | `domain/alarm/warn_policy.c` | 15（三级声光节拍/静音自动解除/手动锁定让位） |
| `test_warn_rule` | `domain/warn_rule.c` | 15（双阈值+防抖/比较方向/中断清零/周期性重触发） |
| `test_json_util` | `domain/json_util.c` | 14（紧凑/带空白/缺键/截断/转义/前缀不误匹配） |

---

## 六、目录总览

```
Edge-AI-Gateway/
├── Makefile                  # 顶层调度（all/core/dts/lvgl/test/clean/deploy）
├── config.mk.example         # 部署路径模板（复制为 config.mk）
├── core/                     # ── 驱动工程 + 网关应用 ──
│   ├── led/ key_event/ gpio_event_capture/ pwm_beep/          # 项目 B 驱动
│   ├── ap3216c/ icm20608/ key_input/ gt9147/
│   ├── Makefile              # 驱动批量编译（KDIR 指向内核源码树）
│   └── lvgl/                 # ── 项目 A 网关应用 ──
│       ├── Makefile          #   交叉编译（链接 ncnn；find src 自动收源）
│       ├── lv_conf.h         #   LVGL 配置（16bpp/部分刷新/内存 256KB）
│       ├── lvgl/             #   LVGL v9.2.2 vendored（仅保留编译所需）
│       └── src/              #   见「一、架构分层」
├── cloud/                    # 云端精判服务（Python）
│   ├── server.py             #   FastAPI + YOLOv8n + face_recognition
│   ├── cloudctl.sh           #   启停脚本
│   ├── cmd_send.py           #   MQTT 命令下发 CLI
│   ├── gen_*.sh              #   密钥/证书生成（api.key / ota.key / TLS / MQTT）
│   ├── make_ota_pkg.sh       #   OTA 打包
│   ├── freeze_deps.sh        #   生成 requirements.lock.txt
│   ├── mosquitto.conf        #   MQTT broker 配置
│   ├── ota/                  #   OTA 包与 manifest
│   ├── secrets/              #   密钥（不入库）
│   ├── certs/                #   CA 证书（不入库）
│   └── whitelist/            #   白名单照片（不入库）
├── models/                   # 板端推理模型（face.param / face.bin）
├── dts/                      # 设备树（imx6ull-southbay-emmc.dts）
├── docs/                     # 设计与验证文档（见「七、文档索引」）
├── tests/                    # PC 端单元测试（10 个用例）
├── tools/fontgen/            # 中文字库生成（Noto Sans SC，OFL）
├── scripts/                  # build_mbedtls.sh
├── third_party/              # ncnn-armhf / mbedtls-armhf 预编译包 + fonts 字库源
└── build/                    # 编译输出（git 忽略）
    ├── module/  dts/  lvgl/  #   结果
    └── cache/                #   过程文件（.o/.d 等）
```

---

## 七、故障排查

| 现象 | 处理 |
|---|---|
| 开机黑屏（背光亮、串口在打印 fps） | App 启动已自动 unblank 自愈；仍黑则重插屏幕排线两端，`dmesg \| grep -iE "mxsfb\|lcd"` |
| 串口刷 `mxs wait for vsync timeout` | fb 处于 blank（息屏）时的正常现象，触摸唤醒即恢复 |
| 息屏唤醒后摄像头画面卡住 | 已修（唤醒后 3s 强制放行 + 每 10 帧兜底）；若仍复现，查 `grep -aE "CAM\|VSYNC" /opt/edge-gateway/logs/gateway.log` |
| 无 `/dev/video0` | 内核缺 `VIDEO_MXC_CSI_CAMERA`（见 3.3，勿用 VIDEO_MXC_CAPTURE） |
| `[DET] load /root/face.param failed` | 模型未部署：`make deploy` 或手动拷 `models/face.*` 到板 `/root/`；缺失时精判回退「有运动即告警」 |
| 云端复核全部 fail-safe 告警 | VM 服务没起/防火墙：`ss -tlnp \| grep 8000`、板端 `ping <VM_IP>` |
| 白名单不生效 | `wget -qO- http://127.0.0.1:8000/whitelist` 确认已加载；照片需露脸正面照 |
| 中文显示成方框 | 新增中文标签后重新生成字库：`cd tools/fontgen && python3 extract_symbols.py && ./generate.sh`，再全量重编 |
| 抓拍丢失 | TF 卡未挂/满卡自动清理（剩余 <50MB 清最旧）；相册重启自动回填 |
| 触摸无响应 | `dmesg \| grep gt9147` 确认加载与中断；App 按驱动名自动探测 eventX |
| PWM 蜂鸣干扰摄像头 | 已知硬件问题：占空比已降至 30%；仍干扰需独立供电/加电容/减振 |

---

## 八、项目 B 进度（驱动工程，全部验收通过）

- [x] P1 LED 字符设备（验收 4/4）· P2 按键中断三种读模式（5/5）· P3 gpio_event 企业级驱动（7/7 + 内核 ftrace）
- [x] P4 PWM/I2C/SPI 子系统驱动 beep_pwm/ap3216c/icm20608（8/8）
- [x] P6-1 input 按键驱动（PASS=10）
- [x] P7-1 GT9147 触摸（中断模式 + 配置自愈）· P5 OV5640 采用内核自带驱动接入

---

## 九、文档索引

| 文档 | 内容 |
|---|---|
| `docs/architecture.md` | **分层规则与迁移状态**（含可复现的依赖审计命令） |
| `docs/refactor-summary.md` | **本轮重构总结**：产物清单、行为等价性、板端必测项 |
| `docs/verification-guide.md` | 阶段验证手册（T1–T13、MQTT、OTA、融合等逐项步骤） |
| `docs/dependencies.md` | 第三方依赖清单、字库来源、Python 版本锁定、已清理项 |
| `docs/locking.md` | 线程/锁模型与内存预算 |
| `docs/performance.md` | 性能实测数据与方法学 |
| `docs/fault-injection.md` | 故障注入测试方法 |
| `docs/error-codes.md` | 统一错误码表 |
| `docs/remaining-work.md` | 剩余工作与既定范围外事项 |
| `cloud/README.md` | 云端服务部署、协议表、白名单管理 |
