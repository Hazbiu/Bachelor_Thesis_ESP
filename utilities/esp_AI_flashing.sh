#!/usr/bin/env bash
set -euo pipefail

# Simple front-end:
#   ./utilities/esp_AI_flashing.sh dl
#   ./utilities/esp_AI_flashing.sh fp32
#   ./utilities/esp_AI_flashing.sh nn
#
# Optional port:
#   ./utilities/esp_AI_flashing.sh nn /dev/ttyACM1

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

MODE="${1:-}"
PORT="${2:-/dev/ttyACM0}"

case "$MODE" in
    dl|espdl|esp-dl)
        BACKEND="dl"
        ;;
    fp32|tflm|tflm-fp32)
        BACKEND="fp32"
        ;;
    nn|int8|esp-nn|tflm-int8|tflm-int8-espnn)
        BACKEND="nn"
        ;;
    *)
        echo "Usage: $0 {dl|fp32|nn} [PORT]"
        echo
        echo "  dl    -> ESP-DL S8"
        echo "  fp32  -> TFLM-FP32"
        echo "  nn    -> TFLM-INT8 + ESP-NN"
        exit 2
        ;;
esac

exec "$PROJECT_ROOT/tools/flash_ai_backend.sh" "$BACKEND" "$PORT"
