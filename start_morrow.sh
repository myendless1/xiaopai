#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

# Runtime configuration
MORROW_BIN="${MORROW_BIN:-$HOME/.local/bin/morrow}"
HOST="${MORROW_HOST:-0.0.0.0}"
PORT="${MORROW_PORT:-3000}"
LOG_FILE="${MORROW_LOG:-/tmp/morrow-server.log}"
HEALTH_HOST="${MORROW_HEALTH_HOST:-127.0.0.1}"
CONFIG_HOME="${XDG_CONFIG_HOME:-$HOME/.config}"
API_KEY_FILE="${MORROW_API_KEY_FILE:-$CONFIG_HOME/xiaopai/morrow-api-key}"

usage() {
  cat >&2 <<EOF
Usage: $0 <lark|nolark|demo>

  lark    Load morrow/config-full.toml and enable Feishu tools with --robot.
  nolark  Load morrow/config-final-event.toml without registering Feishu tools.
  demo    Load morrow/config-demo.toml for the scripted final-event demo.
EOF
}

if [ "$#" -ne 1 ]; then
  usage
  exit 2
fi

MODE="$1"
case "$MODE" in
  lark)
    CONFIG_FILE="$SCRIPT_DIR/morrow/config-full.toml"
    ROBOT_ARGS=(--robot)
    TOOL_MODE="Feishu tools enabled"
    ;;
  nolark)
    CONFIG_FILE="$SCRIPT_DIR/morrow/config-final-event.toml"
    ROBOT_ARGS=()
    TOOL_MODE="Q&A only; Feishu tools not registered"
    ;;
  demo)
    CONFIG_FILE="$SCRIPT_DIR/morrow/config-demo.toml"
    ROBOT_ARGS=()
    TOOL_MODE="Scripted demo only; external tools not registered"
    ;;
  *)
    echo "Error: unsupported mode '$MODE'." >&2
    usage
    exit 2
    ;;
esac

# Keep provider credentials outside the repository. The file may contain
# either the raw key or one OPENAI_API_KEY="..." assignment. It is parsed as
# data and is never sourced as shell code.
API_KEY_DIR="$(dirname -- "$API_KEY_FILE")"
mkdir -p "$API_KEY_DIR"
chmod 700 "$API_KEY_DIR" 2>/dev/null || true
if [ ! -r "$API_KEY_FILE" ]; then
  cat >&2 <<EOF
Error: Morrow API key file is not readable: $API_KEY_FILE

Create it without sudo, then restrict its permissions:
  mkdir -p "$API_KEY_DIR"
  mv /path/to/your/key-file "$API_KEY_FILE"
  chmod 600 "$API_KEY_FILE"

The file may contain either the raw API key or:
  OPENAI_API_KEY="your-api-key"
EOF
  exit 1
fi

KEY_LINE="$(awk 'NF && $1 !~ /^#/ { sub(/\r$/, ""); print; exit }' "$API_KEY_FILE")"
if [[ "$KEY_LINE" =~ ^[[:space:]]*(export[[:space:]]+)?OPENAI_API_KEY[[:space:]]*= ]]; then
  OPENAI_API_KEY_VALUE="${KEY_LINE#*=}"
else
  OPENAI_API_KEY_VALUE="$KEY_LINE"
fi
OPENAI_API_KEY_VALUE="${OPENAI_API_KEY_VALUE#"${OPENAI_API_KEY_VALUE%%[![:space:]]*}"}"
OPENAI_API_KEY_VALUE="${OPENAI_API_KEY_VALUE%"${OPENAI_API_KEY_VALUE##*[![:space:]]}"}"
if [[ "$OPENAI_API_KEY_VALUE" == \"*\" || "$OPENAI_API_KEY_VALUE" == \'*\' ]]; then
  OPENAI_API_KEY_VALUE="${OPENAI_API_KEY_VALUE:1:${#OPENAI_API_KEY_VALUE}-2}"
fi
if [ -z "$OPENAI_API_KEY_VALUE" ]; then
  echo "Error: Morrow API key file is empty: $API_KEY_FILE" >&2
  exit 1
fi
if ! chmod 600 "$API_KEY_FILE" 2>/dev/null; then
  echo "Error: cannot restrict Morrow API key file permissions to 600: $API_KEY_FILE" >&2
  exit 1
fi
export OPENAI_API_KEY="$OPENAI_API_KEY_VALUE"
unset OPENAI_API_KEY_VALUE KEY_LINE

# Morrow must reach its model provider directly. Do not inherit stale proxy
# settings from the interactive shell, env.sh, cron, or a service manager.
unset http_proxy https_proxy all_proxy HTTP_PROXY HTTPS_PROXY ALL_PROXY
export no_proxy="127.0.0.1,localhost${no_proxy:+,$no_proxy}"
export NO_PROXY="127.0.0.1,localhost${NO_PROXY:+,$NO_PROXY}"

if [ ! -x "$MORROW_BIN" ]; then
  if command -v morrow >/dev/null 2>&1; then
    MORROW_BIN="$(command -v morrow)"
  else
    echo "Error: morrow binary not found at $MORROW_BIN and not in PATH." >&2
    exit 1
  fi
fi

if [ ! -r "$CONFIG_FILE" ]; then
  echo "Error: Morrow config is not readable: $CONFIG_FILE" >&2
  exit 1
fi

if ! command -v curl >/dev/null 2>&1; then
  echo "Error: curl is required for the startup health check." >&2
  exit 1
fi

echo "Morrow Server Controller"
echo "  Mode:   $MODE"
echo "  Config: $CONFIG_FILE"
echo "  Tools:  $TOOL_MODE"
echo "  Binary: $MORROW_BIN"
echo "  Host:   $HOST"
echo "  Port:   $PORT"
echo "  Log:    $LOG_FILE"
echo "  Key:    $API_KEY_FILE"
echo "  Proxy:  disabled for Morrow"
echo

# Stop transient units previously used to run Morrow in this workspace.
if command -v systemctl >/dev/null 2>&1; then
  systemctl --user stop \
    morrow-final-qa.service \
    morrow-final-event.service \
    morrow-full-tools-test.service \
    >/dev/null 2>&1 || true
fi

# Find Morrow server processes even when --config appears before `server`.
PIDS="$(
  ps -eo pid=,args= | awk -v bin="$MORROW_BIN" '
    {
      pid = $1
      $1 = ""
      sub(/^ +/, "")
      if (index($0, bin " ") == 1 && $0 ~ /(^| )server( |$)/) {
        print pid
      }
    }
  '
)"

if [ -n "$PIDS" ]; then
  echo "Found existing Morrow server PID(s): $PIDS"
  echo "Gracefully stopping existing processes..."
  for pid in $PIDS; do
    kill "$pid" 2>/dev/null || true
  done

  for _ in {1..10}; do
    running=false
    for pid in $PIDS; do
      if kill -0 "$pid" 2>/dev/null; then
        running=true
        break
      fi
    done
    if [ "$running" = false ]; then
      break
    fi
    sleep 0.5
  done

  for pid in $PIDS; do
    if kill -0 "$pid" 2>/dev/null; then
      echo "Process $pid did not stop; forcing termination."
      kill -9 "$pid" 2>/dev/null || true
    fi
  done
  echo "Existing server stopped."
else
  echo "No existing Morrow server found."
fi

mkdir -p "$(dirname "$LOG_FILE")"
echo "---- morrow-server $MODE start $(date '+%Y-%m-%d %H:%M:%S') ----" >> "$LOG_FILE"

SERVER_CMD=(
  "$MORROW_BIN"
  --config "$CONFIG_FILE"
  server
  "${ROBOT_ARGS[@]}"
  --host "$HOST"
  --port "$PORT"
)

echo "Starting Morrow server..."
if command -v setsid >/dev/null 2>&1; then
  setsid "${SERVER_CMD[@]}" >> "$LOG_FILE" 2>&1 < /dev/null &
else
  nohup "${SERVER_CMD[@]}" >> "$LOG_FILE" 2>&1 < /dev/null &
fi
NEW_PID=$!

STATUS_JSON=""
HEALTH_URL="http://$HEALTH_HOST:$PORT/api/status"
for _ in {1..20}; do
  if ! kill -0 "$NEW_PID" 2>/dev/null; then
    break
  fi
  if STATUS_JSON="$(curl --noproxy '*' -fsS --max-time 1 "$HEALTH_URL" 2>/dev/null)"; then
    break
  fi
  sleep 0.5
done

if ! kill -0 "$NEW_PID" 2>/dev/null; then
  echo "Error: Morrow server failed to start. Latest logs:" >&2
  tail -n 20 "$LOG_FILE" >&2
  exit 1
fi

if [ -z "$STATUS_JSON" ]; then
  echo "Error: Morrow started as PID $NEW_PID but its health endpoint did not respond." >&2
  kill "$NEW_PID" 2>/dev/null || true
  tail -n 20 "$LOG_FILE" >&2
  exit 1
fi

if [[ "$STATUS_JSON" != *"\"config_path\":\"$CONFIG_FILE\""* ]]; then
  echo "Error: Morrow health check reported an unexpected config path." >&2
  echo "$STATUS_JSON" >&2
  kill "$NEW_PID" 2>/dev/null || true
  exit 1
fi

echo "Morrow server started successfully."
echo "  PID:    $NEW_PID"
echo "  Mode:   $MODE"
echo "  Config: $CONFIG_FILE"
echo "  URL:    http://$HOST:$PORT"
echo "  Health: $HEALTH_URL"
echo "  Log:    $LOG_FILE"
echo
echo "Latest logs:"
tail -n 5 "$LOG_FILE"
