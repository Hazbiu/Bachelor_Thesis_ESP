#!/usr/bin/env bash
set -euo pipefail

ROOT="$HOME/Programming/Bachelor_Thesis_ESP"
VENV="$ROOT/tools/.venv-tflite-int8"
SCRIPT="$ROOT/tools/quantize_existing_tflite_to_int8.py"

if [[ ! -f "$SCRIPT" ]]; then
    echo "[ERROR] Missing:"
    echo "  $SCRIPT"
    exit 1
fi

if [[ ! -x "$VENV/bin/python" ]]; then
    echo "[INFO] Creating TensorFlow quantization venv..."
    python3 -m venv "$VENV"
    "$VENV/bin/python" -m pip install --upgrade pip setuptools wheel
    "$VENV/bin/python" -m pip install tensorflow pillow numpy
fi

echo "============================================================"
echo " TFLM INT8 / ESP-NN model conversion"
echo "============================================================"
echo "Detector source : /media/admin/SDCARD/models/FDET32.TFL"
echo "Recognizer src  : /media/admin/SDCARD/models/FREC.TFL"
echo "Calibration     : /media/admin/SDCARD/enroll"
echo
echo "Remove stale UNUSED OperatorCode records before"
echo "TensorFlow calibration."
echo "============================================================"
echo

"$VENV/bin/python" "$SCRIPT" \
    --model-dir /media/admin/SDCARD/models \
    --calibration-dir /media/admin/SDCARD/enroll \
    --samples 160

echo
echo "========== GENERATED =========="
ls -lh \
    /media/admin/SDCARD/models/FDET8.TFL \
    /media/admin/SDCARD/models/FREC8.TFL

echo
echo "========== SHA256 =========="
sha256sum \
    /media/admin/SDCARD/models/FDET8.TFL \
    /media/admin/SDCARD/models/FREC8.TFL
