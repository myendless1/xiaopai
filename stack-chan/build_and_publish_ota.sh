#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SERVER_FIRMWARE_DIR="${STACKCHAN_OTA_FIRMWARE_DIR:-$PROJECT_DIR/stack-chan-server/static/firmware}"
VERSION="${1:-${PROJECT_VER:-}}"

. "$PROJECT_DIR/env.sh"
cd "$PROJECT_DIR"

if [ -n "$VERSION" ]; then
  idf.py -DPROJECT_VER="$VERSION" reconfigure build
else
  idf.py build
fi

read -r APP_BIN PROJECT_VERSION PROJECT_NAME <<EOF
$(python3 - <<'PY'
import json
from pathlib import Path

desc = json.loads(Path("build/project_description.json").read_text())
print(desc["app_bin"], desc["project_version"], desc["project_name"])
PY
)
EOF

APP_PATH="$PROJECT_DIR/build/$APP_BIN"
PUBLISH_VERSION="${VERSION:-$PROJECT_VERSION}"
PUBLISH_NAME="xiaopai-${PUBLISH_VERSION}.bin"
PUBLISH_PATH="$SERVER_FIRMWARE_DIR/$PUBLISH_NAME"

mkdir -p "$SERVER_FIRMWARE_DIR"
cp "$APP_PATH" "$PUBLISH_PATH"

python3 - "$PUBLISH_PATH" "$SERVER_FIRMWARE_DIR/latest.json" "$PUBLISH_VERSION" "$PROJECT_NAME" <<'PY'
import hashlib
import json
import sys
from pathlib import Path

firmware_path = Path(sys.argv[1])
manifest_path = Path(sys.argv[2])
version = sys.argv[3]
project_name = sys.argv[4]
data = firmware_path.read_bytes()
manifest = {
    "version": version,
    "filename": firmware_path.name,
    "size": len(data),
    "sha256": hashlib.sha256(data).hexdigest(),
    "project_name": project_name,
}
manifest_path.write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n")
PY

git -C "$PROJECT_DIR/.." add -- "$SERVER_FIRMWARE_DIR"

printf 'Published OTA firmware:\n'
printf '  version: %s\n' "$PUBLISH_VERSION"
printf '  file:    %s\n' "$PUBLISH_PATH"
printf '  latest:  %s\n' "$SERVER_FIRMWARE_DIR/latest.json"
printf '  staged:  %s\n' "$SERVER_FIRMWARE_DIR"
