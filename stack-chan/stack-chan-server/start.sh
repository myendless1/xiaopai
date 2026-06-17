#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"

HOST="${STACKCHAN_SERVER_HOST:-${STACKCHAN_ALIYUN_HOST:-0.0.0.0}}"
PORT="${STACKCHAN_SERVER_PORT:-${STACKCHAN_ALIYUN_PORT:-8091}}"
VENV="${STACKCHAN_SERVER_VENV:-.venv}"
LOG_FILE="${STACKCHAN_SERVER_LOG:-/tmp/stack-chan-server.log}"
PIP_INDEX_URL="${STACKCHAN_PIP_INDEX_URL:-https://pypi.tuna.tsinghua.edu.cn/simple}"
PIP_TRUSTED_HOST="${STACKCHAN_PIP_TRUSTED_HOST:-pypi.tuna.tsinghua.edu.cn}"
DEBUG="${STACKCHAN_DEBUG:-0}"
SERVER_ARGS=()

while [ "$#" -gt 0 ]; do
  case "$1" in
    --debug)
      DEBUG=1
      shift
      ;;
    *)
      SERVER_ARGS+=("$1")
      shift
      ;;
  esac
done

if [ -f .env ]; then
  set -a
  . ./.env
  set +a
fi

ALIYUN_AK_ID="${ALIYUN_AK_ID:-}"
ALIYUN_AK_SECRET="${ALIYUN_AK_SECRET:-}"
ALIYUN_NLS_APPKEY="${ALIYUN_NLS_APPKEY:-}"
REALTIME_ENABLED="${STACKCHAN_REALTIME_ENABLED:-true}"

for arg in "${SERVER_ARGS[@]}"; do
  case "$arg" in
    --no-realtime-enabled)
      REALTIME_ENABLED=false
      ;;
    --realtime-enabled)
      REALTIME_ENABLED=true
      ;;
  esac
done

if [ ! -x "$VENV/bin/python" ]; then
  python3 -m venv "$VENV"
fi

if ! "$VENV/bin/python" -m pip --version >/dev/null 2>&1; then
  "$VENV/bin/python" -m ensurepip --upgrade
fi

has_requirements() {
  grep -Ev '^[[:space:]]*(#|$)' "$1" >/dev/null 2>&1
}

pip_install() {
  env -u http_proxy -u https_proxy -u all_proxy -u HTTP_PROXY -u HTTPS_PROXY -u ALL_PROXY \
    "$VENV/bin/python" -m pip install \
      -i "$PIP_INDEX_URL" \
      --trusted-host "$PIP_TRUSTED_HOST" \
      -r "$1"
}

if has_requirements requirements.txt; then
  pip_install requirements.txt
fi

if has_requirements requirements-yunet.txt; then
  pip_install requirements-yunet.txt
fi

if [[ ! "$REALTIME_ENABLED" =~ ^(0|false|False|FALSE|no|No|NO|off|Off|OFF)$ ]]; then
  if ! "$VENV/bin/python" - <<'PY' >/dev/null 2>&1
import sys
sys.path.insert(0, "src")
from opus_codec import OpusCodec
codec = OpusCodec(sample_rate=16000)
pcm = b"\x00\x00" * codec.samples_per_frame
codec.decode(codec.encode(pcm))
PY
  then
    echo "Realtime Xiaozhi audio requires Python opuslib and system libopus." >&2
    echo "Python packages are installed from requirements.txt; install the system library, then restart:" >&2
    echo "  sudo apt-get update && sudo apt-get install -y libopus0" >&2
    echo "To run without realtime speech, set STACKCHAN_REALTIME_ENABLED=false or pass --no-realtime-enabled." >&2
    exit 1
  fi
fi

if ! "$VENV/bin/python" - <<'PY' >/dev/null 2>&1
import cv2
import numpy
PY
then
  echo "YuNet dependencies are not installed; image upload works, face boxes may be disabled." >&2
  echo "Install them later with: env -u http_proxy -u https_proxy -u all_proxy $VENV/bin/python -m pip install -i $PIP_INDEX_URL --trusted-host $PIP_TRUSTED_HOST -r requirements-yunet.txt" >&2
fi

if { [ -z "${ALIYUN_NLS_TOKEN:-}" ] && { [ -z "${ALIYUN_AK_ID:-}" ] || [ -z "${ALIYUN_AK_SECRET:-}" ]; }; } || [ -z "${ALIYUN_NLS_APPKEY:-}" ]; then
  echo "Missing Aliyun credentials." >&2
  echo "Set these in stack-chan-server/.env or export them:" >&2
  echo "  ALIYUN_AK_ID='...'" >&2
  echo "  ALIYUN_AK_SECRET='...'" >&2
  echo "  ALIYUN_NLS_APPKEY='...'" >&2
  echo "Alternatively set ALIYUN_NLS_TOKEN instead of ALIYUN_AK_ID/ALIYUN_AK_SECRET." >&2
  exit 1
fi

echo
echo "Xiaopai Server"
echo "  log:          $LOG_FILE"
if [ "$DEBUG" = "1" ] || [ "$DEBUG" = "true" ]; then
  DEBUG_ARGS=(--debug)
  echo "  debug:        enabled"
  echo "  ASR upload:   http://$HOST:$PORT/upload"
  echo "  TTS stream:   http://$HOST:$PORT/stream-speak?text=..."
  echo "  Image upload: http://$HOST:$PORT/upload-image"
  echo "  OTA check:    http://$HOST:$PORT/xiaozhi/ota"
  echo "  OTA latest:   http://$HOST:$PORT/firmware/latest.json"
  echo "  health:       http://127.0.0.1:$PORT/health"
  echo
  echo "Firmware config examples:"
  echo "  CONFIG_STACKCHAN_RECORD_UPLOAD_URL = http://<this-computer-lan-ip>:$PORT/upload"
  echo "  CONFIG_STACKCHAN_STREAM_TTS_URL    = http://<this-computer-lan-ip>:$PORT/stream-speak"
  echo "  CONFIG_STACKCHAN_IMAGE_UPLOAD_URL  = http://<this-computer-lan-ip>:$PORT/upload-image"
  echo "  CONFIG_OTA_URL                     = http://<this-computer-lan-ip>:$PORT/xiaozhi/ota"
else
  DEBUG_ARGS=()
  echo "  debug:        disabled (run ./start.sh --debug for device IDs, task IDs, IPs, ports, and full API bodies)"
fi
echo

mkdir -p "$(dirname "$LOG_FILE")"
echo "---- stack-chan-server start $(date '+%Y-%m-%d %H:%M:%S') ----" >> "$LOG_FILE"

SERVER_CMD=(
  "$VENV/bin/python" src/server.py
  --host "$HOST"
  --port "$PORT"
  "${DEBUG_ARGS[@]}"
  "${SERVER_ARGS[@]}"
)

if command -v setsid >/dev/null 2>&1; then
  setsid env PYTHONUNBUFFERED=1 "${SERVER_CMD[@]}" >> "$LOG_FILE" 2>&1 < /dev/null &
else
  nohup env PYTHONUNBUFFERED=1 "${SERVER_CMD[@]}" >> "$LOG_FILE" 2>&1 < /dev/null &
fi
echo "Started server pid: $!"

echo "Use \`pkill -f -9 server.py\` to stop the server."
