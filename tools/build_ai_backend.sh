#!/usr/bin/env bash
set -euo pipefail

TOOLS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$TOOLS_DIR/.." && pwd)"
SRC_DIR="$PROJECT_ROOT/src_p4"

BACKEND="${1:-}"
if [[ -z "$BACKEND" ]]; then
    echo "Usage: ./tools/build_ai_backend.sh <espdl|tflm-fp32>"
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
echo " Clean build for backend: $BACKEND"
echo "============================================================"

idf.py fullclean
idf.py reconfigure
idf.py build

echo
echo "[OK] Build complete: $BACKEND"
