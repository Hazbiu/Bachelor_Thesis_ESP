#!/usr/bin/env bash
set -euo pipefail

TOOLS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$TOOLS_DIR/.." && pwd)"
SRC_DIR="$PROJECT_ROOT/src"

BACKEND="${1:-}"
PORT="${2:-}"

if [[ -z "$BACKEND" ]]; then
    cat <<'EOF'
Usage:
  ./tools/flash_ai_backend.sh espdl /dev/ttyACM0
  ./tools/flash_ai_backend.sh tflm-fp32 /dev/ttyACM0

The port is optional. If omitted, ESP-IDF will try to detect it.
EOF
    exit 2
fi

"$TOOLS_DIR/select_ai_backend.sh" "$BACKEND"

if ! command -v idf.py >/dev/null 2>&1; then
    IDF_EXPORT="${IDF_EXPORT:-$HOME/esp/esp-idf-v5.5.4/export.sh}"
    if [[ ! -f "$IDF_EXPORT" ]]; then
        echo "[ERROR] idf.py is not in PATH and ESP-IDF export script was not found:"
        echo "        $IDF_EXPORT"
        echo "Set IDF_EXPORT=/path/to/esp-idf/export.sh and run again."
        exit 1
    fi

    # shellcheck disable=SC1090
    source "$IDF_EXPORT"
fi

cd "$SRC_DIR"

echo "============================================================"
echo " Flashing AI backend: $BACKEND"
echo "============================================================"

# Full clean is deliberate: the backend is a compile-time choice and A/B
# measurements should never reuse stale objects from the other backend.
idf.py fullclean
idf.py reconfigure
idf.py build

if [[ -n "$PORT" ]]; then
    idf.py -p "$PORT" flash monitor
else
    idf.py flash monitor
fi
