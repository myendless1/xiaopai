#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(
    cd -- "$(dirname -- "${BASH_SOURCE[0]}")"
    pwd
)"

cd "$SCRIPT_DIR"
source "$SCRIPT_DIR/env.sh"

PORT="${PORT:-/dev/ttyACM0}"
FLASH=false

for arg in "$@"; do
    case "$arg" in
        --flash)
            FLASH=true
            ;;
        -h|--help)
            echo "Usage: $0 [--flash] [PORT]"
            echo "  default: monitor only, without resetting the device"
            echo "  --flash: build and flash before starting the monitor"
            exit 0
            ;;
        --*)
            echo "Unknown option: $arg" >&2
            exit 2
            ;;
        *)
            PORT="$arg"
            ;;
    esac
done

LOG_DIR="${LOG_DIR:-$SCRIPT_DIR/logs/serial}"

mkdir -p "$LOG_DIR"

TIMESTAMP="$(date '+%Y%m%d_%H%M%S')"
LOG_FILE="$LOG_DIR/xiaopai_${TIMESTAMP}.log"

ln -sfn \
    "$(basename "$LOG_FILE")" \
    "$LOG_DIR/latest.log"

echo "Serial port: $PORT"
echo "Log file:    $LOG_FILE"

if "$FLASH"; then
    echo "Building and flashing Xiaopai..."
    "$SCRIPT_DIR/build_and_flash.sh" "$PORT" 2>&1 | tee -a "$LOG_FILE"
fi

echo "Starting serial monitor without resetting the device..." | tee -a "$LOG_FILE"
echo "Exit monitor with Ctrl+]"

idf.py \
    -p "$PORT" \
    monitor \
    --no-reset \
    2>&1 | tee -a "$LOG_FILE"
