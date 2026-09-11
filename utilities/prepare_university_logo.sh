#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd -- "${SCRIPT_DIR}/.." && pwd)"
IMAGE="${ROOT}/src_p4/main/ui/assets/university_logo.jpg"
OUTPUT="${ROOT}/src_p4/main/ui/assets/university_logo.c"
CONVERTER="${ROOT}/tools/convert_gui_logo.py"

if [[ ! -f "${IMAGE}" ]]; then
    echo "ERROR: University logo not found:"
    echo "  ${IMAGE}"
    echo
    echo "Copy your JPG there with the exact filename university_logo.jpg"
    exit 1
fi

PYTHON_BIN="python3"

if ! "${PYTHON_BIN}" -c 'from PIL import Image' >/dev/null 2>&1; then
    echo "Pillow is not installed for python3."
    echo "Install it once with:"
    echo "  python3 -m pip install Pillow"
    exit 2
fi

"${PYTHON_BIN}" "${CONVERTER}" "${IMAGE}" "${OUTPUT}"

echo
echo "[OK] Logo is now compiled as a native LVGL RGB565 asset."
echo "Next flash with:"
echo "  cd ${ROOT}"
echo "  bash utilities/esp_AI_flashing.sh dl /dev/ttyACM0"
