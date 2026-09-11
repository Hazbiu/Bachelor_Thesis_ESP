#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
C6="${ROOT}/src_c6/c6_deep_sleep_firmware"
HOST="${ROOT}/src_c6/p4_c6_direct_flasher"

ACTION="${1:-}"
PORT="${2:-/dev/ttyACM0}"

usage() {
    cat <<'EOF'
Official direct ESP32-P4 -> ESP32-C6 flasher helper

Usage:
  ./utilities/esp_P4_C6_direct.sh prepare
  ./utilities/esp_P4_C6_direct.sh flash   /dev/ttyACM0
  ./utilities/esp_P4_C6_direct.sh monitor /dev/ttyACM0
  ./utilities/esp_P4_C6_direct.sh run     /dev/ttyACM0

Required physical wiring:
  P4 GPIO20 -> C6_U0RXD
  P4 GPIO21 -> C6_U0TXD
  C6_IO9    -> GND

No /dev/ttyUSB0 and no transparent UART bridge are used.

Workflow:
  prepare -> builds C6, creates merged image, embeds it, builds P4 flasher
  run     -> flashes P4 helper and immediately monitors it

First P4-helper boot:
  only BACKS UP the complete C6 factory flash to microSD.
  It does NOT write the C6.

After "C6 FACTORY BACKUP COMPLETE":
  press P4 RESET exactly once.

Second P4-helper boot:
  flashes and MD5-verifies the C6 self-Deep-sleep firmware.
EOF
}

require_idf() {
    if ! command -v idf.py >/dev/null 2>&1; then
        echo "[ERROR] idf.py is not in PATH."
        echo "For this machine the ESP-IDF tree shown in the build logs is:"
        echo "  /home/admin/esp/esp-idf-v5.5.4"
        echo
        echo "Activate it with:"
        echo "  source /home/admin/esp/esp-idf-v5.5.4/export.sh"
        exit 1
    fi
}

prepare_c6() {
    echo "============================================================"
    echo " [1/3] BUILD C6 SELF-DEEP-SLEEP FIRMWARE"
    echo "============================================================"

    cd "$C6"

    if [[ ! -f sdkconfig ]] ||
       ! grep -q 'CONFIG_IDF_TARGET="esp32c6"' sdkconfig; then
        rm -rf build
        rm -f sdkconfig sdkconfig.old
        idf.py set-target esp32c6
    fi

    idf.py build

    echo
    echo "Creating one raw merged ESP32-C6 flash image from ESP-IDF flash_args..."
    echo "[INFO] Running esptool from inside the C6 build directory so every"
    echo "       relative path in flash_args resolves correctly."

    (
        cd "$C6/build"
        python -m esptool \
            --chip esp32c6 \
            merge_bin \
            -o c6_merged.bin \
            -f raw \
            @flash_args
    )

    if [[ ! -f "$C6/build/c6_merged.bin" ]]; then
        echo "[ERROR] Expected merged C6 image was not created:"
        echo "        $C6/build/c6_merged.bin"
        exit 1
    fi

    python - "$C6/build/c6_merged.bin" <<'PY'
from pathlib import Path
import sys

p = Path(sys.argv[1])
data = p.read_bytes()

# esp-serial-flasher requires image_size to be 4-byte aligned.
padding = (-len(data)) % 4
if padding:
    data += b"\xff" * padding
    p.write_bytes(data)

if len(data) < 64 * 1024:
    raise SystemExit(f"[ERROR] merged C6 image suspiciously small: {len(data)} bytes")

if data[0] != 0xE9:
    raise SystemExit(
        f"[ERROR] merged C6 image does not start with ESP image magic 0xE9: "
        f"got 0x{data[0]:02x}"
    )

print(f"[OK] merged C6 image: {len(data)} bytes, 4-byte aligned")
PY

    cp "$C6/build/c6_merged.bin" "$HOST/main/c6_merged.bin"
    sha256sum "$HOST/main/c6_merged.bin"
}

prepare_host() {
    echo
    echo "============================================================"
    echo " [2/3] CONFIGURE P4 DIRECT FLASHER FOR REVISION v1.3"
    echo "============================================================"

    cd "$HOST"

    if [[ ! -f sdkconfig ]] ||
       ! grep -q 'CONFIG_IDF_TARGET="esp32p4"' sdkconfig ||
       ! grep -q '^CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y$' sdkconfig; then
        rm -rf build
        rm -f sdkconfig sdkconfig.old
        idf.py set-target esp32p4
    fi

    if ! grep -q '^CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y$' sdkconfig; then
        echo "[ERROR] P4 pre-v3 compatibility not active."
        exit 1
    fi

    echo "[OK] CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y"
}

build_host() {
    echo
    echo "============================================================"
    echo " [3/3] BUILD OFFICIAL P4 -> C6 DIRECT FLASHER"
    echo "============================================================"

    cd "$HOST"
    idf.py build

    echo
    echo "============================================================"
    echo " PREPARE COMPLETE"
    echo "============================================================"
    echo "Next:"
    echo "  $0 run /dev/ttyACM0"
}

do_prepare() {
    require_idf
    prepare_c6
    prepare_host
    build_host
}

do_flash() {
    require_idf

    [[ -e "$PORT" ]] || {
        echo "[ERROR] Port not found: $PORT"
        exit 1
    }

    [[ -f "$HOST/build/p4_c6_direct_flasher.bin" ]] || {
        echo "[ERROR] P4 direct flasher is not built."
        echo "Run:"
        echo "  $0 prepare"
        exit 1
    }

    echo "============================================================"
    echo " FLASHING P4 DIRECT C6 PROGRAMMER"
    echo "============================================================"
    echo
    echo "Keep connected:"
    echo "  GPIO20 -> C6_U0RXD"
    echo "  GPIO21 -> C6_U0TXD"
    echo "  C6_IO9 -> GND"
    echo
    echo "Make sure a writable microSD card is inserted."
    echo

    cd "$HOST"
    idf.py -p "$PORT" flash
}

do_monitor() {
    require_idf
    cd "$HOST"
    idf.py -p "$PORT" monitor
}

do_run() {
    do_flash
    echo
    echo "Starting monitor..."
    echo "Exit ESP-IDF monitor with Ctrl+]."
    echo
    cd "$HOST"
    idf.py -p "$PORT" monitor
}

case "$ACTION" in
    prepare) do_prepare ;;
    flash)   do_flash ;;
    monitor) do_monitor ;;
    run)     do_run ;;
    *)
        usage
        exit 2
        ;;
esac
