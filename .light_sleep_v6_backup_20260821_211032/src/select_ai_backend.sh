#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
TOOLS_DIR="$PROJECT_ROOT/tools"
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
    echo "[ERROR] Missing backend selector:"
    echo "  $TOOLS_DIR/select_ai_backend.sh"
    exit 1
fi

"$TOOLS_DIR/select_ai_backend.sh" "$CANONICAL"

echo
echo "========== V5 LIGHT-SLEEP CHECK =========="
grep -q 'LIGHT_SLEEP_TOUCH_FIX version=5' \
    "$SCRIPT_DIR/main/power/app_sleep.c"
grep -q 'APP_LIGHT_SLEEP_TOUCH_STARTUP_IGNORE_MS' \
    "$SCRIPT_DIR/main/include/config/app_config.h"
grep -q 'APP_LIGHT_SLEEP_TOUCH_RELEASE_STABLE_MS' \
    "$SCRIPT_DIR/main/include/config/app_config.h"
grep -q 'APP_LIGHT_SLEEP_TOUCH_PRESS_STABLE_MS' \
    "$SCRIPT_DIR/main/include/config/app_config.h"
echo "  V5 time-qualified touch gate : present"

# Activate the user's ESP-IDF 5.5.4 installation when necessary.
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

if [[ -z "${IDF_PATH:-}" || ! -f "${IDF_PATH}/tools/idf.py" ]]; then
    echo "[ERROR] IDF_PATH is invalid after activation: ${IDF_PATH:-<unset>}"
    exit 1
fi

PYTHON_BIN="$(command -v python || command -v python3)"
IDF=("$PYTHON_BIN" "$IDF_PATH/tools/idf.py")

# Auto-detect the ESP serial device if a port was not provided.
if [[ -z "$PORT" ]]; then
    if [[ -d /dev/serial/by-id ]]; then
        PORT="$(find -L /dev/serial/by-id -maxdepth 1 -type c -o -type l 2>/dev/null | sort | head -n1 || true)"
    fi
fi
if [[ -z "$PORT" ]]; then
    PORT="$(find /dev -maxdepth 1 \( -name 'ttyACM*' -o -name 'ttyUSB*' \) -print 2>/dev/null | sort | head -n1 || true)"
fi
if [[ -z "$PORT" || ! -e "$PORT" ]]; then
    echo "[ERROR] No ESP32-P4 serial port found."
    echo "Run: ls -l /dev/ttyACM* /dev/ttyUSB* /dev/serial/by-id/* 2>/dev/null"
    exit 1
fi

if [[ "$CANONICAL" == "tflm-int8" ]]; then
    if [[ ! -f "$SCRIPT_DIR/sdkconfig" ]] ||
       ! grep -q '^CONFIG_NN_OPTIMIZED=y' "$SCRIPT_DIR/sdkconfig"; then
        echo "[ERROR] NN backend requires CONFIG_NN_OPTIMIZED=y in:"
        echo "  $SCRIPT_DIR/sdkconfig"
        exit 1
    fi
fi

echo
echo "============================================================"
echo " ESP32-P4 V5 clean build / flash"
echo "============================================================"
echo "Backend : $LABEL"
echo "Port    : $PORT"
echo "Project : $SCRIPT_DIR"
echo "============================================================"

# Force a clean build so neither the old V4 touch state machine nor a previous
# AI backend can survive in build artifacts.
rm -rf "$SCRIPT_DIR/build"

cd "$SCRIPT_DIR"
"${IDF[@]}" reconfigure
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
