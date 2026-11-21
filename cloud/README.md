# 云端精判服务（YOLOv8 人员检测 + 人脸白名单比对 + OTA 包分发）

接收网关上传的 RGB565 帧，两级判定：
1. **YOLOv8n** 判定画面中是否有**人员**；
2. 有人时用 **face_recognition**（dlib）做人脸比对 `whitelist/` 白名单。

返回 JSON 结论，设备端按结论告警。
对应设备端：`core/lvgl/src/cloud_detect.c`（识别模式=云端精判 时启用）。

同一进程还提供 **OTA 包下载**（`/ota/`），与精判共用 8000 端口；
MQTT broker 由 `cloudctl.sh` 一起拉起（1883）。

## 安装（VM/主机）

新 Ubuntu（Python 3.12+）pip 受 PEP 668 管控，**必须用虚拟环境**，且 `face_recognition`
依赖的 `pkg_resources` 在 setuptools≥81 已被移除，需先钉住旧版（08-30 板测实证）：

```bash
# ① 虚拟环境（一次性）
python3 -m venv ~/cloud-venv
source ~/cloud-venv/bin/activate          # 每次开新终端跑服务前都要先执行

# ② 先装 setuptools<81（否则 face_recognition 报"Please install face_recognition_models"，
#    实际是 pkg_resources 缺失，报错信息有误导性）
pip install "setuptools<81" -i https://pypi.tuna.tsinghua.edu.cn/simple

# ③ 依赖清单
cd cloud
pip install -r requirements.txt -i https://pypi.tuna.tsinghua.edu.cn/simple

# ④ 验证（应打印 FR OK；pkg_resources 弃用警告无害）
python -c "import face_recognition; print('FR OK')"
```

说明：
- `yolov8n.pt`（6.2MB）已预置在本目录并随仓库分发，**运行不依赖外网**；
  若需更换版本，从 https://github.com/ultralytics/assets/releases 下载后覆盖同名文件
- `face_recognition` 依赖 dlib，pip 安装时需本地编译数分钟（需 cmake/g++，
  编译机已具备）；**模型权重随 pip 包内置，安装后运行不依赖外网**
- 若 dlib 编译失败：服务仍可运行（人员检测正常），但所有人按陌生人处理（不漏报）

## 运行

```bash
cd cloud
chmod +x cloudctl.sh make_ota_pkg.sh gen_ota_keys.sh gen_mqtt_passwd.sh   # 一次性
./cloudctl.sh start      # 启动 精判+OTA(8000) 与 MQTT(1883)
./cloudctl.sh status     # 进程/端口/health/chrony
./cloudctl.sh logs       # tail -f 两个日志
./cloudctl.sh restart    # 改配置/账号后重启
./cloudctl.sh stop
```

> `start` 内部用 `nohup` 启动，**关闭终端不会退出**；只有手动 `stop` 或 VM 重启后才需要重新 `start`。
> 确认仍在运行：`ss -tln | grep -E ':(8000|1883)'`。

`cloudctl.sh` 会自动：

- 优先使用 `~/cloud-venv/bin/python3`（若无则用系统 `python3`）；
- 检查 8000/1883 端口占用、系统 `mosquitto` 服务冲突；
- 日志写到 `cloud/logs/cloud.log`、`cloud/logs/mosquitto.log`；
- 提示 chrony 是否在运行（NTP 授时，板端时间同步依赖它）。

手动单独启动也可以用：

```bash
cd cloud
uvicorn server:app --host 0.0.0.0 --port 8000
```

确认监听：`ss -tlnp | grep -E ':(8000|1883)'`

## OTA 包（与精判同端口）

```bash
cd cloud
./make_ota_pkg.sh 1.2.0                     # 默认取 ../build/lvgl/lvgl_gateway
# 或指定二进制：./make_ota_pkg.sh 1.2.0 /path/to/lvgl_gateway
```

生成：

```
cloud/ota/pkg-1.2.0.tar.gz    # manifest.json + payload.tar.gz + checksums.sha256
cloud/ota/manifest.json       # 版本 + 包名 + sha256
```

板端有两种触发方式（详见 `docs/verification-guide.md` 3.15）：

```bash
# ① 启动/定时拉取
GATEWAY_OTA_URL=http://192.168.3.26:8000/ota/manifest.json \
GATEWAY_OTA_INTERVAL_MIN=0 /opt/edge-gateway/current/lvgl_gateway &

# ② 远程下发（推荐，只在需要时触发）
mosquitto_pub -h 127.0.0.1 -t 'gateway/gw-<MAC>/cmd' \
  -m '{"cmd":"ota","id":7,"url":"http://192.168.3.26:8000/ota/manifest.json"}'
```

> `GATEWAY_OTA_INTERVAL_MIN` 缺省为 0：不设置时**只有 MQTT 下发才会升级**。

## HTTP 鉴权（T11-B2）

```bash
cd cloud
./gen_api_key.sh              # 生成 secrets/api.key（600），打印 Key
./cloudctl.sh restart         # server.py 启动时读取
```

- 除 `/health`（与 `/docs`）外，所有接口都要求请求头 `X-API-Key: <key>`；
- 未配置 `secrets/api.key` / `CLOUD_API_KEY` 时不校验（启动时打印警告，仅限内网开发）；
- 板端把 Key 写入 `gateway.json` 的 `cloud_api_key`（或 `set_config` 下发），
  应用会自动带给 `/detect` 与 OTA 下载请求；
- 命令行测试：

```bash
curl -H "X-API-Key: <key>" http://127.0.0.1:8000/whitelist
```

## MQTT TLS（T11-③，8883）

```bash
cd cloud
./gen_tls_certs.sh            # 生成 certs/ca.crt + server.crt/key（SAN 默认 192.168.3.26）
./cloudctl.sh restart         # 同时监听 1883（明文）与 8883（TLS 1.2）
```

- 单向认证：板端用 `ca.crt` 校验服务器证书，客户端仍用用户名/密码；
- 板端配置（`gateway.json`）：
  `"mqtt_port": 8883, "mqtt_tls": 1, "mqtt_ca_path": "/opt/edge-gateway/certs/ca.crt"`；
- `make deploy` 会把 `certs/ca.crt` 复制到板端 `/opt/edge-gateway/certs/`；
- 自检：

```bash
mosquitto_sub -h 127.0.0.1 -p 8883 --cafile certs/ca.crt \
  -u gateway -P '<密码>' -t 'gateway/#' -v -C 1 -W 3
```

> 自签证书仅用于开发；生产应换成正式 CA 签发的证书（SAN 必须包含连接用的 IP/域名）。

## 命令下发（ACK + 超时重试，T12 精简版）

```bash
pip install paho-mqtt          # 或 ~/cloud-venv/bin/pip install -r requirements.lock.txt
echo 'Gw#2026pass' > secrets/mqtt.pass && chmod 600 secrets/mqtt.pass   # broker 密码（CLI 与 /cmd 共用）

# CLI（paho 装在 cloud-venv）
~/cloud-venv/bin/python3 ./cmd_send.py --device gw-b8ae1d010000 --cmd set_config \
  --params '{"brightness":70}' --timeout 2 --retries 3

# HTTP（走 X-API-Key）
curl -s -X POST http://127.0.0.1:8000/cmd \
  -H "X-API-Key: $(cat secrets/api.key)" -H 'Content-Type: application/json' \
  -d '{"device":"gw-b8ae1d010000","cmd":"set_config","params":{"brightness":70}}'
```

- 设备端 ACK：`gateway/{id}/cmd/ack`，内容 `{"id":N,"cmd":"...","ok":true}`；
- 超时未收到同 id ACK 会重发（默认 2s × 3 次）；
- 设备端对最近 16 个命令 id 去重：重试只补 ACK，不会重复执行（OTA 不会被重复触发）。

## 白名单管理

每人一张**露脸正面照**（jpg/png），文件名即姓名：

```
cloud/whitelist/张三.jpg
cloud/whitelist/李四.png
```

- 目录文件有增删/修改，服务**自动重载**（无需重启），控制台打印
  `[cloud] 白名单已加载 N 人: ...`
- 也可用 HTTP 添加（与目录投放等效）：
  `curl -X POST "http://127.0.0.1:8000/whitelist/add?name=张三" --data-binary @张三.jpg`
- 查看已加载：`curl http://127.0.0.1:8000/whitelist`
- 白名单为空 / face_recognition 不可用时，人员一律按**陌生人**处理（fail-safe 不漏报）
- 比对容差 `FACE_TOLERANCE = 0.6`（server.py 顶部可调，越小越严）

## 协议

```
POST /detect?w=<宽>&h=<高>
Content-Type: application/octet-stream
body   = RGB565 原始帧（w*h*2 字节，小端，行连续）

响应 200: {"person": bool, "known": bool, "names": [姓名...], "type": str, "count": N, "conf": F}
```

`type` = 本帧最高置信度检测类别的归一键：`person`（人）/ `animal`（猫狗等动物）/
`object`（其他移动物）。设备端映射为"人员/动物/物体"写入时间轴。

| 云端结论 | 条件 | 设备端行为（统一两级管线，云端=复核定案） |
|---|---|---|
| 非人员 | `person=false`（type=animal/object/空） | 不告警；若本地已告警→**自动消警**（本地误报被压掉）；时间轴记录类型 |
| 已授权 | `person=true, known=true` | 白名单命中；若本地已告警→**自动消警**；时间轴"告警解除" |
| 陌生人 | `person=true, known=false` | STRANGER 告警（本地未告警则升级） |
| 不可达 | 非 200/超时/连接失败 | fail-safe：本地未告警且未检出人脸→INTRUDER 告警；已告警→维持（不漏报） |

设备端为**两级管线**：本地 SCRFD 先初判（检出人脸立即告警/未检出仅轻提醒），
云端复核定案（白名单自动消警/陌生人确认/误报撤销）。`GET /whitelist`（查看白名单）、
`POST /whitelist/add?name=姓名`（添加）。

## 防火墙

VM 内开放 8000 端口（或临时关闭 ufw）：`sudo ufw allow 8000/tcp`

## 板端地址

`core/lvgl/src/cloud_detect.c` 的 `CLOUD_DETECT_SERVER`（默认 `192.168.3.26:8000`，
即 VM 的 IP——与 NFS/TFTP 同机）。修改后需重编应用。

注：设备端 v1 不解析返回的 `names` 中文姓名（字库为业务子集，任意姓名可能缺字），
时间轴统一显示"已授权：人员"；姓名解析留到云端管理页批次。
