#!/bin/sh
# ============================================================
# cloudctl.sh — 云端统一控制（精判 + OTA 静态 + MQTT broker）
#
#   云端精判 + OTA 文件   http://<VM>:8000   （server.py，FastAPI）
#   MQTT broker           <VM>:1883           （mosquitto）
#   NTP 授时              UDP 123             （chrony，系统服务，仅检查）
#
# 用法：
#   ./cloudctl.sh start      启动（uvicorn + mosquitto）
#   ./cloudctl.sh stop       停止
#   ./cloudctl.sh restart
#   ./cloudctl.sh status     查看端口/进程/健康检查
#   ./cloudctl.sh logs       跟踪日志
# ============================================================
set -u

BASE=$(cd "$(dirname "$0")" && pwd)
LOGS="$BASE/logs"
PID_HTTP="$LOGS/cloud.pid"
PID_MQTT="$LOGS/mosquitto.pid"
PORT_HTTP=8000
PORT_MQTT=1883
PORT_MQTT_TLS=8883

mkdir -p "$LOGS" 2>/dev/null

# 优先使用 cloud/README.md 里创建的虚拟环境
if [ -x "$HOME/cloud-venv/bin/python3" ]; then
    PY="$HOME/cloud-venv/bin/python3"
else
    PY=python3
fi

is_run() {
    [ -f "$1" ] || return 1
    pid=$(cat "$1" 2>/dev/null)
    [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null
}

port_busy() {
    # BusyBox 的 ss 可能不支持 -p，用 grep 端口即可
    ss -tln 2>/dev/null | grep -q ":$1 "
}

start_http() {
    if is_run "$PID_HTTP"; then
        echo "cloud   : already running (pid $(cat "$PID_HTTP"))"
        return 0
    fi
    if port_busy "$PORT_HTTP"; then
        echo "cloud   : port $PORT_HTTP already in use (别的 http.server/uvicorn?)"
        return 1
    fi
    ( cd "$BASE" && exec nohup "$PY" -m uvicorn server:app --host 0.0.0.0 --port "$PORT_HTTP" ) \
        >> "$LOGS/cloud.log" 2>&1 &
    echo $! > "$PID_HTTP"

    # 等端口就绪（首次启动要加载 YOLO 模型，可能 5~15s）
    i=0
    while [ "$i" -lt 30 ]; do
        if ! is_run "$PID_HTTP"; then
            echo "cloud   : 进程已退出，见 $LOGS/cloud.log"
            return 1
        fi
        if port_busy "$PORT_HTTP"; then
            echo "cloud   : started :$PORT_HTTP (pid $(cat "$PID_HTTP"), log $LOGS/cloud.log)"
            return 0
        fi
        sleep 1
        i=$((i + 1))
    done
    echo "cloud   : 进程在但 30s 内未监听 $PORT_HTTP（模型加载慢？）见 $LOGS/cloud.log"
    return 0
}

start_mqtt() {
    if is_run "$PID_MQTT"; then
        echo "mqtt    : already running (pid $(cat "$PID_MQTT"))"
        return 0
    fi
    if command -v systemctl >/dev/null 2>&1 && systemctl is-active --quiet mosquitto 2>/dev/null; then
        echo "mqtt    : 系统 mosquitto 服务占用了 $PORT_MQTT，请先: sudo systemctl stop mosquitto"
        return 1
    fi
    if port_busy "$PORT_MQTT"; then
        echo "mqtt    : port $PORT_MQTT already in use"
        return 1
    fi
    if [ ! -f "$BASE/secrets/mosquitto.passwd" ]; then
        echo "mqtt    : 缺少 $BASE/secrets/mosquitto.passwd（先运行 ./gen_mqtt_passwd.sh [user] [pass]）"
        return 1
    fi
    if [ ! -f "$BASE/certs/server.crt" ]; then
        echo "mqtt    : 提示：certs/ 未生成，8883 TLS 会启动失败（./gen_tls_certs.sh）"
    fi
    ( cd "$BASE" && exec nohup mosquitto -c mosquitto.conf ) \
        >> "$LOGS/mosquitto.log" 2>&1 &
    echo $! > "$PID_MQTT"
    sleep 1
    if is_run "$PID_MQTT"; then
        echo "mqtt    : started :$PORT_MQTT / :$PORT_MQTT_TLS TLS (pid $(cat "$PID_MQTT"), log $LOGS/mosquitto.log)"
    else
        echo "mqtt    : start FAILED, see $LOGS/mosquitto.log"
        return 1
    fi
}

stop_one() {
    name=$1
    pidfile=$2
    if is_run "$pidfile"; then
        pid=$(cat "$pidfile")
        kill "$pid" 2>/dev/null
        sleep 1
        kill -0 "$pid" 2>/dev/null && kill -9 "$pid" 2>/dev/null
        echo "$name    : stopped (pid $pid)"
    else
        echo "$name    : not running"
    fi
    rm -f "$pidfile"
}

case "${1:-start}" in
start)
    start_http
    start_mqtt
    echo "---- status ----"
    ss -tln 2>/dev/null | grep -E ":($PORT_HTTP|$PORT_MQTT|$PORT_MQTT_TLS) " || true
    if command -v systemctl >/dev/null 2>&1 && systemctl is-active --quiet chrony 2>/dev/null; then
        echo "ntp     : chrony active (UDP 123)"
    else
        echo "ntp     : chrony 未运行（板端时间同步会失败）: sudo systemctl start chrony"
    fi
    ;;
stop)
    stop_one "cloud" "$PID_HTTP"
    stop_one "mqtt " "$PID_MQTT"
    ;;
restart)
    "$0" stop
    sleep 1
    "$0" start
    ;;
status)
    if is_run "$PID_HTTP"; then
        echo "cloud   : running (pid $(cat "$PID_HTTP"))"
    elif port_busy "$PORT_HTTP"; then
        echo "cloud   : running (systemd/other, port $PORT_HTTP)"
    else
        echo "cloud   : stopped"
    fi
    if is_run "$PID_MQTT"; then
        echo "mqtt    : running (pid $(cat "$PID_MQTT"))"
    elif port_busy "$PORT_MQTT"; then
        echo "mqtt    : running (systemd/other, port $PORT_MQTT)"
    else
        echo "mqtt    : stopped"
    fi
    ss -tln 2>/dev/null | grep -E ":($PORT_HTTP|$PORT_MQTT|$PORT_MQTT_TLS) " || echo "ports   : 未监听"
    if command -v systemctl >/dev/null 2>&1 && systemctl is-active --quiet chrony 2>/dev/null; then
        echo "ntp     : chrony active"
    else
        echo "ntp     : chrony inactive"
    fi
    echo "---- /health ----"
    "$PY" - <<'EOF'
import json, urllib.request
try:
    with urllib.request.urlopen("http://127.0.0.1:8000/health", timeout=2) as r:
        print(json.dumps(json.load(r), ensure_ascii=False))
except Exception as e:
    print("health check failed:", e)
EOF
    ;;
logs)
    tail -n 40 -f "$LOGS/cloud.log" "$LOGS/mosquitto.log"
    ;;
*)
    echo "usage: $0 {start|stop|restart|status|logs}"
    exit 1
    ;;
esac
