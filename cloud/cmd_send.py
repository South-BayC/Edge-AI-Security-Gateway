#!/usr/bin/env python3
"""
cmd_send.py — 云端命令下发：ACK 等待 + 超时重试（T12 精简版）

背景：
    设备端收到 gateway/{id}/cmd 后，会向 gateway/{id}/cmd/ack 回复
    {"id":N,"cmd":"...","ok":true}。
    本模块负责：发布命令 → 等待同 id 的 ACK → 超时重发（默认 2s × 3 次）。
    设备端对重复 id 只补 ACK、不重复执行，因此重试是幂等的。

用法：
    python3 cmd_send.py --device gw-b8ae1d010000 \
        --cmd set_config --params '{"brightness":70}'
    python3 cmd_send.py --device gw-xxx --cmd set_warn \
        --params '{"zone":3,"src":2,"threshold":30,"threshold_warn":15}'

环境变量：MQTT_BROKER / MQTT_PORT / MQTT_USER / MQTT_PASS
"""
import argparse
import json
import os
import threading
import time

try:
    import paho.mqtt.client as mqtt
except ImportError:  # 延迟到调用时再报错，方便只 import 本模块的单元测试
    mqtt = None


BROKER = os.environ.get("MQTT_BROKER", "127.0.0.1")
PORT = int(os.environ.get("MQTT_PORT", "1883"))
USER = os.environ.get("MQTT_USER", "gateway")
CA_FILE = os.environ.get("MQTT_CA", "")   # 非空则走 TLS（8883）


def _load_password():
    """优先环境变量 MQTT_PASS，其次 cloud/secrets/mqtt.pass（600）。"""
    pw = os.environ.get("MQTT_PASS", "")
    if pw:
        return pw
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "secrets", "mqtt.pass")
    if not os.path.isfile(path):
        path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "mqtt.pass")
    if os.path.isfile(path):
        try:
            with open(path, "r", encoding="utf-8") as f:
                return f.read().strip()
        except OSError:
            return ""
    return ""


PASSWORD = _load_password()


class AckClient:
    """订阅所有设备 ACK，并按命令 id 唤醒等待者。"""

    def __init__(self):
        self.client = None
        self.lock = threading.Lock()
        self.pending = {}          # id -> (Event, [ack_dict])
        self.connected = False

    def start(self):
        if mqtt is None:
            raise RuntimeError("paho-mqtt 未安装（pip install paho-mqtt）")
        if self.client is not None:
            return self
        try:
            client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
        except AttributeError:      # paho-mqtt 1.x
            client = mqtt.Client()
        if USER:
            client.username_pw_set(USER, PASSWORD)
        if CA_FILE:
            client.tls_set(ca_certs=CA_FILE)
            client.tls_insecure_set(False)
        client.on_message = self._on_message
        client.on_connect = self._on_connect
        client.connect(BROKER, PORT, keepalive=30)
        client.subscribe("gateway/+/cmd/ack", qos=0)
        client.loop_start()
        self.client = client
        return self

    def _on_connect(self, client, userdata, flags, rc, properties=None):
        # paho 1.x: rc 是 int；2.x(VERSION2): rc 是 ReasonCode
        ok = (rc == 0) if isinstance(rc, int) else (not rc.is_failure)
        self.connected = ok
        if ok:
            client.subscribe("gateway/+/cmd/ack", qos=0)

    def _on_message(self, client, userdata, msg):
        try:
            data = json.loads(msg.payload.decode("utf-8", "replace"))
        except Exception:
            return
        cid = data.get("id")
        with self.lock:
            waiter = self.pending.get(cid)
        if waiter:
            waiter[1].append(data)
            waiter[0].set()

    def expect(self, cmd_id):
        ev = threading.Event()
        box = []
        with self.lock:
            self.pending[cmd_id] = (ev, box)
        return ev, box

    def release(self, cmd_id):
        with self.lock:
            self.pending.pop(cmd_id, None)

    def close(self):
        if self.client is not None:
            self.client.loop_stop()
            self.client.disconnect()
            self.client = None


_CLIENT = None
_CLIENT_LOCK = threading.Lock()


def _client():
    global _CLIENT
    with _CLIENT_LOCK:
        if _CLIENT is None:
            _CLIENT = AckClient().start()
        return _CLIENT


def send_command(device_id, cmd, params=None, timeout=2.0, retries=3, cmd_id=None):
    """
    发布命令并等待 ACK。

    :return: {"ok": bool, "attempts": int, "ack": dict|None, "id": int}
    """
    params = dict(params or {})
    if cmd_id is None:
        cmd_id = int(time.time() * 1000) % 2147483647
    payload = {"cmd": cmd, "id": cmd_id}
    payload.update(params)
    # 压紧 JSON：设备端解析器只做最小实现，避免空格带来的兼容问题
    body = json.dumps(payload, ensure_ascii=False, separators=(",", ":"))

    cli = _client()
    ev, box = cli.expect(cmd_id)
    topic = "gateway/%s/cmd" % device_id
    attempts = 0
    try:
        for _ in range(retries + 1):
            attempts += 1
            info = cli.client.publish(topic, body, qos=0)
            if info.rc != 0:
                continue
            if ev.wait(timeout):
                ack = box[0] if box else None
                return {
                    "ok": bool(ack.get("ok", False)) if ack else False,
                    "attempts": attempts,
                    "ack": ack,
                    "id": cmd_id,
                }
        return {"ok": False, "attempts": attempts, "ack": None, "id": cmd_id}
    finally:
        cli.release(cmd_id)


def main():
    ap = argparse.ArgumentParser(description="MQTT 命令下发（ACK + 超时重试）")
    ap.add_argument("--device", required=True, help="设备 ID，如 gw-b8ae1d010000")
    ap.add_argument("--cmd", required=True, help="命令名，如 set_config/set_warn/ota")
    ap.add_argument("--params", default="{}", help="命令参数 JSON")
    ap.add_argument("--timeout", type=float, default=2.0, help="单次 ACK 超时秒数")
    ap.add_argument("--retries", type=int, default=3, help="超时重试次数")
    ap.add_argument("--id", type=int, default=None, help="指定命令 id（默认按时间生成）")
    args = ap.parse_args()

    try:
        params = json.loads(args.params)
    except json.JSONDecodeError as e:
        raise SystemExit("--params 不是合法 JSON: %s" % e)

    res = send_command(args.device, args.cmd, params,
                       timeout=args.timeout, retries=args.retries, cmd_id=args.id)
    print(json.dumps(res, ensure_ascii=False))
    raise SystemExit(0 if res["ok"] else 2)


if __name__ == "__main__":
    main()
