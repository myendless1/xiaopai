#!/usr/bin/env bash
set -euo pipefail

PORT="${1:-/dev/ttyACM0}"
MODE="${2:-}"
PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

. "$PROJECT_DIR/env.sh"
cd "$PROJECT_DIR"

idf.py build

(
  cd build
  python -m esptool \
    --chip esp32s3 \
    -p "$PORT" \
    -b 460800 \
    --before default_reset \
    --after hard_reset \
    write_flash "@flash_args"
)

printf '\nBuild and flash complete. Board hard reset attempted automatically.\n'

if [ "$MODE" = "monitor" ]; then
  idf.py -p "$PORT" monitor
fi
