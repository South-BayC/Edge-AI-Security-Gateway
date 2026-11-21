# 故障注入与恢复验证（D4）

> 汇总本项目做过的故障注入实验：场景、命令、预期、实测、结论。
> 设备：i.MX6ULL + Buildroot，NFS rootfs，云端 VM（192.168.3.26）。
> 证据来源：`docs/verification-guide.md` 各章节与板端日志。

---

## 1. 传感器通信失败

| 项 | 内容 |
|---|---|
| 场景 | AP3216C 掉线 |
| 命令 | `rmmod ap3216c; sleep 2` |
| 预期 | 连续 3 次读失败 → `COMM_ERR`，关闭失效 fd，1s 后重连 |
| 实测 | `[SENSOR] AP3216C fault: COMM_ERR`（约 1.5s） |
| 恢复 | `insmod /lib/modules/4.1.15/ap3216c.ko` → `AP3216C reconnected` + `recovered` |
| 结论 | 通过；`invalid_cnt` 清零后不再过度敏感 |

## 2. IMU buffer 故障

| 项 | 内容 |
|---|---|
| 场景 | 设备从 SPI 总线消失（`rmmod` 报 EBUSY，改用 unbind） |
| 命令 | `echo spi2.0 > /sys/bus/spi/drivers/icm20608/unbind` |
| 预期 | `buffer read -1` → sysfs 回退 → 3 次失败 → `COMM_ERR` |
| 实测 | `[IMU] buffer read -1 bytes: No such file or directory` → `ICM20608 fault: COMM_ERR`（约 0.3s） |
| 恢复 | `echo spi2.0 > .../bind` → `buffer enabled` + `recovered` + `reconnected` |
| 结论 | 通过；reconnected 可能比 recovered 晚约 0.3s（探测节拍），属正常 |

## 3. 采集线程卡死（KEY0 evdev 阻塞）

| 项 | 内容 |
| --- | --- |
| 场景 | 按一次 KEY0 后采集线程永久阻塞 |
| 证据 | `/proc/<pid>/task/*/wchan` 显示 `evdev_read`；AP3216C 报 `STALE` 且永不恢复 |
| 根因 | `open(/dev/input/eventX, O_RDONLY)` 阻塞模式，内层 `read` 读空后挂住 |
| 修复 | `O_NONBLOCK` + `EAGAIN` 退出循环；增加 `KEY0 event` 日志 |
| 验证 | 9 次按键后采集线程存活，49 秒无 STALE |
| 结论 | 通过（详见 verification-guide 3.18） |

## 4. 断网缓存与补传

| 项 | 内容 |
|---|---|
| 场景 | MQTT broker 停止 |
| 命令 | VM：`pkill mosquitto`；板端触发事件（传感器注入） |
| 预期 | `events.jsonl` 持续增长、不丢；恢复后自动补传归零 |
| 实测 | 队列稳定 4 条不丢；broker 恢复后 ≤60s（退避上限）清空 |
| 重启补传 | 断网时重启：`pending=2` → 恢复后 0 |
| 结论 | 通过 |

## 5. MQTT 认证失败

| 项 | 内容 |
|---|---|
| 场景 | 错误用户名/密码 |
| 命令 | `GATEWAY_MQTT_USER=gateway GATEWAY_MQTT_PASS=wrong ...` |
| 预期 | 连接被拒，仅重试，不 onoffline 抖动 |
| 实测 | `auth enabled` + `connect ... failed` 循环；无 `connected` |
| 结论 | 通过 |

## 6. HTTP 鉴权

| 项 | 内容 |
|---|---|
| 场景 | 未携带 `X-API-Key` 调用云端接口 |
| 命令 | `curl`/python 请求 `/whitelist` |
| 预期 | 401；带 Key 200 |
| 实测 | `no-key -> 401`、`with-key -> 200 {"names":["SZH"]}`；OTA 下载正常 |
| 结论 | 通过 |

## 7. OTA 包完整性

| 场景 | 构造方式 | 预期 | 实测 |
|---|---|---|---|
| 正常包 | `make_ota_pkg.sh` | `hmac ok` → 安装 | 通过（1.4.0/1.5.0） |
| hmac 被篡改 | 改 manifest 的 hmac | `hmac mismatch` → 拒绝，版本不变 | 通过 |
| 缺 hmac 字段 | 删除 manifest 的 hmac | `manifest missing "hmac"` → 拒绝 | 通过 |
| 包内容被改 | 追加字节 | 先被 sha256 拦下 | 未单独构造（逻辑在 hmac 之前） |

## 8. 坏版本自动回滚（真实故障）

| 项 | 内容 |
|---|---|
| 场景 | 2.4.0 因结构体 ABI 不一致在启动阶段崩溃 |
| 现象 | 连续两次启动未确认（`PENDING_CONFIRM`），第三次启动自动回滚 |
| 实测 | `new version not confirmed after boot, rolling back` → `rolled back to 2.2.0` |
| 结论 | 通过；验证了 OTA 的 `boot_count` 保护 |

## 9. 看门狗

| 项 | 内容 |
|---|---|
| 场景 | 进程异常退出/卡死 |
| 命令 | `kill -9 $(pidof lvgl_gateway)`（未 magic close） |
| 预期 | 30s（应用设置）后系统复位 |
| 实测 | `imx2-wdt ... Expect reboot!` → 系统重启 |
| 结论 | 通过 |

## 10. 单实例

| 项 | 内容 |
|---|---|
| 场景 | 重复启动 |
| 命令 | 第二次执行 `/opt/edge-gateway/current/lvgl_gateway` |
| 预期 | `another instance is running`，退出码 1 |
| 实测 | 日志 + `exit=1` |
| 结论 | 通过 |

## 11. 摄像头光照突变

| 项 | 内容 |
|---|---|
| 场景 | 开灯/关灯 |
| 预期 | 只出现 `light jump reset`，不产生 `motion hit` |
| 实测 | 关灯无输出；开灯仅 `light jump reset (thr=10)`；行人正常 `motion hit` |
| 结论 | 通过（T4 三场景） |

## 12. 静态/行人对照

| 场景 | 预期 | 实测 |
|---|---|---|
| 静态 30 分钟 | 无 `motion hit` | 通过（多次 13~30 分钟观察） |
| 人走过 | 出现 `motion hit area=2xx/4000` | 通过 |
| 无人但有影子/窗帘 | 不告警 | 依赖 ROI/阴影抑制（C4，待验证） |

---

## 13. 汇总

| 类别 | 用例数 | 通过 | 待验证 |
|---|---|---|---|
| 传感器/线程 | 3 | 3 | 0 |
| 网络/MQTT | 3 | 3 | 0 |
| OTA | 5 | 4 | 1（sha256 篡改） |
| 系统 | 2 | 2 | 0 |
| 摄像头 | 3 | 2 | 1（C4 ROI/阴影） |

> 汇总表随新实验更新；每次注入请在 `verification-guide` 对应章节补一行记录。
