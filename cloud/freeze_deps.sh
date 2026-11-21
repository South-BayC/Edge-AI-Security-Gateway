#!/bin/sh
# 固定 Python 依赖版本：把当前 venv 的精确版本写入 requirements.lock.txt
#
# 用法（开发机 VM）：
#   ~/cloud-venv/bin/pip install -r requirements.txt
#   ./freeze_deps.sh
#   git add requirements.lock.txt
#
# 之后部署/复现：
#   ~/cloud-venv/bin/pip install -r requirements.lock.txt
set -e
BASE=$(cd "$(dirname "$0")" && pwd)
PY="$HOME/cloud-venv/bin/python3"
[ -x "$PY" ] || PY=python3

"$PY" -m pip freeze | grep -Ei '^(fastapi|uvicorn|ultralytics|numpy|face-recognition|paho-mqtt|opencv|torch|torchvision)==' \
    > "$BASE/requirements.lock.txt"

echo "已写入 $BASE/requirements.lock.txt"
cat "$BASE/requirements.lock.txt"
