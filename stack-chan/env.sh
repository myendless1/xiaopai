#!/usr/bin/env bash

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

find_idf_path() {
  local candidate
  for candidate in \
    "$PROJECT_DIR/esp-idf" \
    "$PROJECT_DIR/../esp-idf" \
    "$PROJECT_DIR/../../esp-idf" \
    "$PROJECT_DIR/../../stack-chan/esp-idf"
  do
    if [ -f "$candidate/export.sh" ]; then
      cd "$candidate" && pwd
      return 0
    fi
  done

  return 1
}

if [ -n "${IDF_PATH:-}" ]; then
  if [ ! -f "$IDF_PATH/export.sh" ]; then
    printf 'IDF_PATH is set, but export.sh was not found: %s\n' "$IDF_PATH" >&2
    return 1 2>/dev/null || exit 1
  fi
  IDF_PATH="$(cd "$IDF_PATH" && pwd)"
else
  if ! IDF_PATH="$(find_idf_path)"; then
    cat >&2 <<EOF
ESP-IDF was not found.
Looked in these paths relative to this script:
  esp-idf
  ../esp-idf
  ../../esp-idf
  ../../stack-chan/esp-idf

Set IDF_PATH to an ESP-IDF checkout, or place esp-idf next to env.sh.
EOF
    return 1 2>/dev/null || exit 1
  fi
fi

IDF_BASE_DIR="$(cd "$IDF_PATH/.." && pwd)"

export IDF_PATH
if [ -z "${IDF_TOOLS_PATH:-}" ]; then
  if [ -d "$PROJECT_DIR/.espressif" ] || [ "$IDF_BASE_DIR" = "$PROJECT_DIR" ]; then
    export IDF_TOOLS_PATH="$PROJECT_DIR/.espressif"
  else
    export IDF_TOOLS_PATH="$IDF_BASE_DIR/.espressif"
  fi
fi
if [ -z "${IDF_PYTHON_ENV_PATH:-}" ]; then
  # Let ESP-IDF select its managed venv when no checkout-local venv exists.
  if [ -d "$PROJECT_DIR/.venv" ]; then
    export IDF_PYTHON_ENV_PATH="$PROJECT_DIR/.venv"
  elif [ -d "$IDF_BASE_DIR/.venv" ]; then
    export IDF_PYTHON_ENV_PATH="$IDF_BASE_DIR/.venv"
  fi
fi
export UV_CACHE_DIR="${UV_CACHE_DIR:-$PROJECT_DIR/.uv-cache}"
export XDG_CACHE_HOME="${XDG_CACHE_HOME:-$PROJECT_DIR/.cache}"


# Use the LAN mixed proxy by default; caller-provided values still take precedence.
export http_proxy="${http_proxy:-http://10.254.48.24:12001}"
export https_proxy="${https_proxy:-http://10.254.48.24:12001}"
export all_proxy="${all_proxy:-http://10.254.48.24:12001}"

. "$IDF_PATH/export.sh"
