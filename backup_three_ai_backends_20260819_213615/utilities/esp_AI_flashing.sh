#!/usr/bin/env bash
set -euo pipefail

# Usage:
#   ./esp_AI_flashing.sh dl
#   ./esp_AI_flashing.sh nn
#
# Optional serial port:
#   ./esp_AI_flashing.sh dl /dev/ttyACM0
#   ./esp_AI_flashing.sh nn /dev/ttyACM0

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

MODE="${1:-}"
PORT="${2:-/dev/ttyACM0}"

case "$MODE" in
    dl|espdl)
        BACKEND="espdl"
        LABEL="ESP-DL"
        ;;
    nn|tflm|tflm-fp32)
        BACKEND="tflm-fp32"
        LABEL="TFLM-FP32"
        ;;
    *)
        echo "Usage:"
        echo "  $0 dl [PORT]    # Flash ESP-DL"
        echo "  $0 nn [PORT]    # Flash TFLM-FP32"
        echo
        echo "Examples:"
        echo "  $0 dl"
        echo "  $0 nn"
        echo "  $0 dl /dev/ttyACM0"
        echo "  $0 nn /dev/ttyACM0"
        exit 1
        ;;
esac

FLASH_SCRIPT="$PROJECT_ROOT/tools/flash_ai_backend.sh"

if [[ ! -x "$FLASH_SCRIPT" ]]; then
    echo "[ERROR] Flash script not found or not executable:"
    echo "  $FLASH_SCRIPT"
    exit 1
fi

echo "============================================================"
echo " ESP32-P4 AI Backend Flasher"
echo "============================================================"
echo "Backend : $LABEL"
echo "Mode    : $MODE"
echo "Port    : $PORT"
echo "Project : $PROJECT_ROOT"
echo "============================================================"
echo

cd "$PROJECT_ROOT"

exec "$FLASH_SCRIPT" "$BACKEND" "$PORT"