#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
TOOLS_DIR="$PROJECT_ROOT/tools"
PATCHER="$SCRIPT_DIR/tools/patch_gt911_stale_points.py"

MODE="${1:-nn}"
PORT="${2:-}"

case "$MODE" in
    nn|int8|esp-nn|tflm-int8|tflm-int8-espnn)
        CANONICAL="tflm-int8"
        LABEL="TFLM-INT8 + ESP-NN"
        ;;
    dl|espdl|esp-dl)
        CANONICAL="espdl"
        LABEL="ESP-DL S8"
        ;;
    fp32|tflm|tflm-fp32)
        CANONICAL="tflm-fp32"
        LABEL="TFLM-FP32"
        ;;
    *)
        echo "Usage: ./select_ai_backend.sh {nn|dl|fp32} [PORT]"
        exit 2
        ;;
esac

if [[ ! -x "$TOOLS_DIR/select_ai_backend.sh" ]]; then
    echo "[ERROR] Missing existing backend selector:"
    echo "  $TOOLS_DIR/select_ai_backend.sh"
    exit 1
fi

"$TOOLS_DIR/select_ai_backend.sh" "$CANONICAL"

echo
echo "========== V6 LIGHT-SLEEP CHECK =========="
grep -q 'LIGHT_SLEEP_TOUCH_FIX version=6' \
    "$SCRIPT_DIR/main/power/app_sleep.c"
grep -q 'GT911_FRESH_DATA_CACHE_FIX' \
    "$SCRIPT_DIR/main/power/app_sleep.c"
echo "  V6 application touch policy : present"

if [[ -z "${IDF_PATH:-}" || ! -f "${IDF_PATH}/tools/idf.py" ]]; then
    ACTIVATE="$HOME/.espressif/tools/activate_idf_v5.5.4.sh"
    EXPORT="$HOME/.espressif/v5.5.4/esp-idf/export.sh"
    if [[ -f "$ACTIVATE" ]]; then
        # shellcheck disable=SC1090
        source "$ACTIVATE"
    elif [[ -f "$EXPORT" ]]; then
        # shellcheck disable=SC1090
        source "$EXPORT"
    else
        echo "[ERROR] ESP-IDF 5.5.4 activation script not found."
        exit 1
    fi
fi

PYTHON_BIN="$(command -v python || command -v python3)"
IDF=("$PYTHON_BIN" "$IDF_PATH/tools/idf.py")

if [[ -z "$PORT" ]]; then
    if [[ -d /dev/serial/by-id ]]; then
        PORT="$(find -L /dev/serial/by-id -maxdepth 1 \( -type c -o -type l \) -print 2>/dev/null | sort | head -n1 || true)"
    fi
fi
if [[ -z "$PORT" ]]; then
    PORT="$(find /dev -maxdepth 1 \( -name 'ttyACM*' -o -name 'ttyUSB*' \) -print 2>/dev/null | sort | head -n1 || true)"
fi
if [[ -z "$PORT" || ! -e "$PORT" ]]; then
    echo "[ERROR] No ESP32-P4 serial port found."
    exit 1
fi

if [[ "$CANONICAL" == "tflm-int8" ]]; then
    grep -q '^CONFIG_NN_OPTIMIZED=y' "$SCRIPT_DIR/sdkconfig" || {
        echo "[ERROR] CONFIG_NN_OPTIMIZED=y is required for NN."
        exit 1
    }
fi

echo
echo "============================================================"
echo " ESP32-P4 V6 clean build / flash"
echo "============================================================"
echo "Backend : $LABEL"
echo "Port    : $PORT"
echo "Project : $SCRIPT_DIR"
echo "============================================================"

rm -rf "$SCRIPT_DIR/build"
cd "$SCRIPT_DIR"

# Configure first so Component Manager has materialized the exact managed driver
# version selected by dependencies.lock.
"${IDF[@]}" reconfigure

GT911_SRC="$(find "$SCRIPT_DIR/managed_components" -type f -name 'esp_lcd_touch_gt911.c' -print -quit 2>/dev/null || true)"
if [[ -z "$GT911_SRC" ]]; then
    GT911_SRC="$(find "$SCRIPT_DIR" -type f -name 'esp_lcd_touch_gt911.c' -print -quit 2>/dev/null || true)"
fi
if [[ -z "$GT911_SRC" ]]; then
    echo "[ERROR] Could not locate esp_lcd_touch_gt911.c after reconfigure."
    exit 1
fi

echo
echo "========== GT911 DRIVER ROOT FIX =========="
echo "Driver : $GT911_SRC"
"$PYTHON_BIN" "$PATCHER" "$GT911_SRC"
grep -q 'LIGHT_SLEEP_GT911_INVALIDATE_STALE_POINTS_V6' "$GT911_SRC"
echo "  stale cached points on DATA_READY=0 : fixed"

"${IDF[@]}" build

if [[ "$CANONICAL" == "tflm-int8" ]]; then
    map_file="$(find build -maxdepth 1 -type f -name '*.map' -print -quit)"
    if [[ -z "$map_file" ]] || ! grep -qi 'esp-nn' "$map_file"; then
        echo "[ERROR] NN build completed but ESP-NN linkage was not found."
        exit 1
    fi
    echo "[OK] ESP-NN linkage verified in $(basename "$map_file")"
fi

"${IDF[@]}" -p "$PORT" flash

if [[ "${NO_MONITOR:-0}" == "1" ]]; then
    echo "[OK] Flash complete; monitor skipped."
    exit 0
fi

echo
echo "Starting monitor. Press Ctrl+] to exit."
exec "${IDF[@]}" -p "$PORT" monitor
