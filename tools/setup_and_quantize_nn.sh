#!/usr/bin/env bash
set -euo pipefail

ROOT="${HOME}/Programming/Bachelor_Thesis_ESP"
VENV="${ROOT}/tools/.venv-tflite-int8"
SCRIPT="${ROOT}/tools/quantize_existing_tflite_to_int8.py"

if [[ ! -f "$SCRIPT" ]]; then
    echo "[ERROR] Missing: $SCRIPT"
    exit 1
fi

python3 -m venv "$VENV"
"$VENV/bin/python" -m pip install --upgrade pip setuptools wheel
"$VENV/bin/python" -m pip install --upgrade tensorflow pillow numpy

"$VENV/bin/python" "$SCRIPT" \
    --model-dir /media/admin/SDCARD/models \
    --calibration-dir /media/admin/SDCARD/enroll \
    --samples 160

echo
echo "========== GENERATED MODELS =========="
ls -lh \
    /media/admin/SDCARD/models/FDET8.TFL \
    /media/admin/SDCARD/models/FREC8.TFL

echo
echo "========== SHA256 =========="
sha256sum \
    /media/admin/SDCARD/models/FDET8.TFL \
    /media/admin/SDCARD/models/FREC8.TFL
