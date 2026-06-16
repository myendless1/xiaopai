#!/usr/bin/env bash
set -euo pipefail

PORT="${1:-/dev/ttyACM0}"
MODE="${2:-}"
PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

. "$PROJECT_DIR/env.sh"
cd "$PROJECT_DIR"

if [ ! -f "$PROJECT_DIR/build/flash_args" ]; then
  cat >&2 <<EOF
Flash arguments were not found at build/flash_args.
Run ./build_and_flash.sh first so the firmware is configured and built.
EOF
  exit 1
fi

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

printf '\nFlash complete. Board hard reset attempted automatically.\n'

if [ "$MODE" = "monitor" ]; then
  idf.py -p "$PORT" monitor
fi
