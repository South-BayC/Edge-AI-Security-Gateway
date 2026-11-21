# 统一错误码表（B4）

> 目的：把各模块的失败原因标准化，日志中可直接检索（如 `grep "OTA:HMAC"`）。
> 源码：`core/lvgl/src/error_code.h` / `error_code.c`

---

## 1. 编码规则

```
code = -(模块号 << 8 | 错误号)
```

- 返回值约定不变：`0` = 成功，负值 = 失败；
- 日志中用 `err_str(code)` 打印可读名，例如：

```c
LOGE("OTA", "hmac check failed [%s]", err_str(ERR_OTA_HMAC));
// 输出: hmac check failed [OTA:HMAC]
```

- 模块号：

| 编号 | 模块 | 编号 | 模块 |
|---|---|---|---|
| 1 | CFG | 7 | OTA |
| 2 | LOG | 8 | TIME |
| 3 | SENSOR | 9 | BRIDGE |
| 4 | CAM | 10 | HEALTH |
| 5 | DET | 11 | CLOUD（HTTP） |
| 6 | MQTT | | |

---

## 2. 错误码

| 错误码 | 名称 | 含义 | 已接入 |
|---|---|---|---|
| `ERR_CFG_PATH` | `CFG:PATH` | 配置目录不可写 | — |
| `ERR_CFG_PARSE` | `CFG:PARSE` | JSON 解析失败（回退默认） | — |
| `ERR_CFG_WRITE` | `CFG:WRITE` | 原子写失败 | — |
| `ERR_LOG_INIT` | `LOG:INIT` | 日志初始化失败 | — |
| `ERR_LOG_WRITE` | `LOG:WRITE` | 落盘/轮转失败 | — |
| `ERR_SENSOR_COMM` | `SENSOR:COMM` | IIO 读失败（连续升级） | — |
| `ERR_SENSOR_STALE` | `SENSOR:STALE` | 超时无新样本 | — |
| `ERR_SENSOR_RANGE` | `SENSOR:RANGE` | 越界 | — |
| `ERR_SENSOR_SPIKE` | `SENSOR:SPIKE` | 变化率/3σ 剔除 | — |
| `ERR_CAM_OPEN` | `CAM:OPEN` | `/dev/video0` 打开失败 | — |
| `ERR_CAM_FMT` | `CAM:FMT` | S_FMT/S_PARM 失败 | — |
| `ERR_CAM_STREAM` | `CAM:STREAM` | STREAMON/DQBUF 失败 | — |
| `ERR_CAM_FRAME` | `CAM:FRAME` | 转换/发布异常 | — |
| `ERR_DET_QUEUE` | `DET:QUEUE` | 精判队列满丢弃 | — |
| `ERR_DET_NO_FRAME` | `DET:NO_FRAME` | 无帧可抓 | — |
| `ERR_DET_MODEL` | `DET:MODEL` | 模型缺失/加载失败 | — |
| `ERR_MQTT_CONNECT` | `MQTT:CONNECT` | TCP/CONNACK 失败 | — |
| `ERR_MQTT_AUTH` | `MQTT:AUTH` | 用户名密码错误 | — |
| `ERR_MQTT_SEND` | `MQTT:SEND` | 发布失败 | — |
| `ERR_MQTT_RECV` | `MQTT:RECV` | 收包/解析失败 | — |
| `ERR_OTA_MANIFEST` | `OTA:MANIFEST` | manifest 拉取/解析失败 | ✅ |
| `ERR_OTA_FETCH` | `OTA:FETCH` | 包下载失败 | ✅ |
| `ERR_OTA_SHA` | `OTA:SHA` | 外层 sha256 不匹配 | ✅ |
| `ERR_OTA_HMAC` | `OTA:HMAC` | HMAC 校验失败/缺失 | ✅ |
| `ERR_OTA_EXTRACT` | `OTA:EXTRACT` | 解包失败 | ✅ |
| `ERR_OTA_INSTALL` | `OTA:INSTALL` | 安装/切换失败 | ✅ |
| `ERR_TIME_RESOLVE` | `TIME:RESOLVE` | NTP 地址解析失败 | — |
| `ERR_TIME_RECV` | `TIME:RECV` | 无响应/超时 | — |
| `ERR_TIME_SET` | `TIME:SET` | settimeofday 失败 | — |
| `ERR_BRIDGE_IIO` | `BRIDGE:IIO` | IIO 设备缺失 | — |
| `ERR_BRIDGE_KEY` | `BRIDGE:KEY` | 按键设备缺失 | — |
| `ERR_CLOUD_CONNECT` | `CLOUD:CONNECT` | 云端 HTTP 连接失败 | ✅ |
| `ERR_CLOUD_STATUS` | `CLOUD:STATUS` | 非 200（含 401） | ✅ |
| `ERR_CLOUD_AUTH` | `CLOUD:AUTH` | 鉴权失败 | — |

> “已接入”表示日志中已带该错误码；其余错误码已定义，按需在对应模块接入。

---

## 3. 验证方式

- OTA 负向测试时日志应出现：

```
[ERROR] [OTA] hmac mismatch [OTA:HMAC]: got=... want=...
```

- 断网/服务不可达时应出现：

```
[ERROR] [HTTP] connect 192.168.3.26:8000 failed [CLOUD:CONNECT]: ...
```

检索示例：

```bash
grep -E "\[(OTA|CLOUD|SENSOR):" /opt/edge-gateway/logs/gateway.log | tail -20
```

---

## 4. 约定

1. 新代码失败时返回 `ERR_*`（负值），不要返回裸 `-1`；
2. 日志格式统一为 `... [模块:错误]`，便于 grep；
3. 新增模块号/错误号时同步更新本表；
4. 不改变既有对外接口的成功/失败语义，只细化错误原因。
