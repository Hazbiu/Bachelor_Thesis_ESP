#!/usr/bin/env bash
set -euo pipefail

SRC_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SRC_DIR/.." && pwd)"
TOOLS_SELECTOR="$PROJECT_ROOT/tools/select_ai_backend.sh"
PATCHER="$SRC_DIR/tools/patch_gt911_stale_points.py"

MODE="${1:-nn}"
PORT="${2:-}"

case "$MODE" in
  nn|int8|esp-nn|tflm-int8|tflm-int8-espnn)
    SELECT_ARG="tflm-int8"
    EXPECTED_HEADER="APP_AI_BACKEND_TFLM_INT8"
    LABEL="TFLM-INT8 + ESP-NN"
    ;;
  dl|espdl|esp-dl)
    SELECT_ARG="espdl"
    EXPECTED_HEADER="APP_AI_BACKEND_ESPDL"
    LABEL="ESP-DL"
    ;;
  fp32|tflm|tflm-fp32)
    SELECT_ARG="tflm-fp32"
    EXPECTED_HEADER="APP_AI_BACKEND_TFLM_FP32"
    LABEL="TFLM-FP32"
    ;;
  *)
    echo "Usage: ./select_ai_backend.sh {nn|dl|fp32} [PORT]"
    exit 2
    ;;
esac

[[ -x "$TOOLS_SELECTOR" ]] || {
  echo "[ERROR] Missing $TOOLS_SELECTOR"
  exit 1
}

"$TOOLS_SELECTOR" "$SELECT_ARG"

HEADER="$SRC_DIR/main/include/config/ai_backend_selection.h"
grep -q "$EXPECTED_HEADER" "$HEADER" || {
  echo "[ERROR] Backend selector did not produce $EXPECTED_HEADER"
  cat "$HEADER"
  exit 1
}

echo
echo "========== V8 LIGHT-SLEEP SOURCE CHECK =========="
grep -q 'LIGHT_SLEEP_TOUCH_FIX version=8' "$SRC_DIR/main/power/app_sleep.c"
grep -q 'DOC_ALIGNED_GT911_POLLING_AUX_POWERDOWN' "$SRC_DIR/main/power/app_sleep.c"
grep -q 'LIGHT_SLEEP_AUX_SUSPENDED' "$SRC_DIR/main/power/app_sleep.c"
echo "  V8 documented Light-sleep policy: present"

for cfg in \
  CONFIG_PM_ENABLE \
  CONFIG_PM_SLP_IRAM_OPT \
  CONFIG_ESP_SLEEP_FLASH_LEAKAGE_WORKAROUND \
  CONFIG_ESP_SLEEP_PSRAM_LEAKAGE_WORKAROUND
do
  grep -q "^${cfg}=y$" "$SRC_DIR/sdkconfig" || {
    echo "[ERROR] $cfg=y is required by the V8 package."
    exit 1
  }
done

grep -q '^# CONFIG_ESP_SLEEP_POWER_DOWN_FLASH is not set$' "$SRC_DIR/sdkconfig" || {
  echo "[ERROR] Flash supply power-down must stay disabled because PSRAM is used."
  exit 1
}

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

if [[ -z "$PORT" && -d /dev/serial/by-id ]]; then
  PORT="$(find -L /dev/serial/by-id -maxdepth 1 \( -type c -o -type l \) -print 2>/dev/null | sort | head -n1 || true)"
fi
if [[ -z "$PORT" ]]; then
  PORT="$(find /dev -maxdepth 1 \( -name 'ttyACM*' -o -name 'ttyUSB*' \) -print 2>/dev/null | sort | head -n1 || true)"
fi
[[ -n "$PORT" && -e "$PORT" ]] || {
  echo "[ERROR] ESP32-P4 serial port not found."
  exit 1
}

if [[ "$SELECT_ARG" == "tflm-int8" ]]; then
  grep -q '^CONFIG_NN_OPTIMIZED=y' "$SRC_DIR/sdkconfig" || {
    echo "[ERROR] CONFIG_NN_OPTIMIZED=y is required for NN."
    exit 1
  }
fi

echo
echo "============================================================"
echo " ESP32-P4 V8 clean build / flash"
echo "============================================================"
echo "Backend : $LABEL"
echo "Port    : $PORT"
echo "Sleep   : documented manual Light-sleep + GT911 polling"
echo "Aux     : audio OFF, SD OFF, Ethernet RESET during Light-sleep"
echo "============================================================"

rm -rf "$SRC_DIR/build"
cd "$SRC_DIR"
"${IDF[@]}" reconfigure

GT911_SRC="$(find "$SRC_DIR/managed_components" -type f -name 'esp_lcd_touch_gt911.c' -print -quit 2>/dev/null || true)"
if [[ -z "$GT911_SRC" ]]; then
  GT911_SRC="$(find "$SRC_DIR" -type f -name 'esp_lcd_touch_gt911.c' -print -quit 2>/dev/null || true)"
fi
[[ -n "$GT911_SRC" ]] || {
  echo "[ERROR] Could not locate esp_lcd_touch_gt911.c"
  exit 1
}

"$PYTHON_BIN" "$PATCHER" "$GT911_SRC"
grep -q 'LIGHT_SLEEP_GT911_INVALIDATE_STALE_POINTS_V6' "$GT911_SRC"
echo "[OK] GT911 stale cached-point fix present."

"${IDF[@]}" build

if [[ "$SELECT_ARG" == "tflm-int8" ]]; then
  MAP="$(find build -maxdepth 1 -type f -name '*.map' -print -quit)"
  [[ -n "$MAP" ]] && grep -qi 'esp-nn' "$MAP" || {
    echo "[ERROR] NN build completed but ESP-NN was not found in the map."
    exit 1
  }
  echo "[OK] ESP-NN linkage verified."
fi

"${IDF[@]}" -p "$PORT" flash

if [[ "${NO_MONITOR:-0}" == "1" ]]; then
  exit 0
fi

exec "${IDF[@]}" -p "$PORT" monitor
