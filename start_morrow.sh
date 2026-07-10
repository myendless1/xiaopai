#!/usr/bin/env bash
set -euo pipefail

# Configuration
MORROW_BIN="${MORROW_BIN:-/home/myendless/.local/bin/morrow}"
HOST="${MORROW_HOST:-0.0.0.0}"
PORT="${MORROW_PORT:-3000}"
LOG_FILE="${MORROW_LOG:-/tmp/morrow-server.log}"

# Morrow must reach its model provider directly. Do not inherit stale proxy
# settings (for example localhost:7890) from the interactive shell, env.sh,
# cron, or a service manager.
unset http_proxy https_proxy all_proxy HTTP_PROXY HTTPS_PROXY ALL_PROXY
export no_proxy="127.0.0.1,localhost${no_proxy:+,$no_proxy}"
export NO_PROXY="127.0.0.1,localhost${NO_PROXY:+,$NO_PROXY}"

# Ensure morrow binary exists
if [ ! -x "$MORROW_BIN" ]; then
  if command -v morrow >/dev/null 2>&1; then
    MORROW_BIN="$(command -v morrow)"
  else
    echo "Error: morrow binary not found at $MORROW_BIN and not in PATH." >&2
    exit 1
  fi
fi

echo "Morrow Server Controller"
echo "  Binary: $MORROW_BIN"
echo "  Host:   $HOST"
echo "  Port:   $PORT"
echo "  Log:    $LOG_FILE"
echo "  Proxy:  disabled for Morrow"
echo

# 1. Clean up existing morrow server processes if any
# We find processes that contain the binary path and 'server' argument
PIDS=$(pgrep -f "$(basename "$MORROW_BIN") server" || true)

if [ -n "$PIDS" ]; then
  echo "Found existing Morrow server running with PID(s): $PIDS"
  echo "Gracefully stopping existing processes..."
  for pid in $PIDS; do
    kill "$pid" 2>/dev/null || true
  done

  # Wait up to 5 seconds for them to stop
  for i in {1..10}; do
    if ! pgrep -f "$(basename "$MORROW_BIN") server" >/dev/null; then
      break
    fi
    sleep 0.5
  done

  # Force kill if still running
  PIDS_STILL=$(pgrep -f "$(basename "$MORROW_BIN") server" || true)
  if [ -n "$PIDS_STILL" ]; then
    echo "Existing processes did not stop. Forcing termination of: $PIDS_STILL"
    for pid in $PIDS_STILL; do
      kill -9 "$pid" 2>/dev/null || true
    done
    sleep 1
  fi
  echo "Existing server stopped."
else
  echo "No existing Morrow server found."
fi

# 2. Start new morrow server in the background
mkdir -p "$(dirname "$LOG_FILE")"
echo "---- morrow-server start $(date '+%Y-%m-%d %H:%M:%S') ----" >> "$LOG_FILE"

SERVER_CMD=(
  "$MORROW_BIN" server
  --robot
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

# 3. Verify if server started successfully
sleep 1.5

if kill -0 "$NEW_PID" 2>/dev/null; then
  echo "Morrow server started successfully."
  echo "  PID:  $NEW_PID"
  echo "  URL:  http://$HOST:$PORT"
  echo "  Log:  $LOG_FILE"
  echo
  echo "Latest logs:"
  tail -n 5 "$LOG_FILE"
else
  echo "Error: Morrow server failed to start. Check logs below:" >&2
  tail -n 20 "$LOG_FILE" >&2
  exit 1
fi
