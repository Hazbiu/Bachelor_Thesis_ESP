#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
C6_PROJECT="$ROOT/c6_deep_sleep_firmware"
BACKUP_DIR="$ROOT/c6_factory_backups"

ACTION="${1:-}"
PORT="${2:-/dev/ttyACM0}"

usage() {
    cat <<'EOF'
ESP32-C6 helper through the temporary ESP32-P4 UART bridge

Usage:
  ./utilities/esp_C6_sleep_flashing.sh probe   /dev/ttyACM0
  ./utilities/esp_C6_sleep_flashing.sh backup  /dev/ttyACM0
  ./utilities/esp_C6_sleep_flashing.sh build
  ./utilities/esp_C6_sleep_flashing.sh flash   /dev/ttyACM0
  ./utilities/esp_C6_sleep_flashing.sh restore /dev/ttyACM0 /path/to/c6_factory_full_flash.bin

Required wiring:
  P4 GPIO20 -> C6_U0RXD
  P4 GPIO21 -> C6_U0TXD
  C6_IO9    -> GND

The P4 must currently be running p4_c6_uart_bridge.
The raw bridge runs at 115200 baud.
EOF
}

require_port() {
    if [[ ! -e "$PORT" ]]; then
        echo "[ERROR] Serial port does not exist: $PORT"
        ls -l /dev/ttyUSB* /dev/ttyACM* 2>/dev/null || true
        exit 1
    fi
}

require_tools() {
    command -v idf.py >/dev/null 2>&1 || {
        echo "[ERROR] idf.py is not available."
        echo "Activate ESP-IDF first, e.g.:"
        echo "  source ~/.espressif/tools/activate_idf_v5.5.4.sh"
        exit 1
    }

    python -m esptool version >/dev/null 2>&1 || {
        echo "[ERROR] Python esptool is not available."
        exit 1
    }
}

prepare_c6_target() {
    cd "$C6_PROJECT"

    if [[ -f sdkconfig ]] &&
       grep -q 'CONFIG_IDF_TARGET="esp32c6"' sdkconfig; then
        return
    fi

    rm -rf build
    rm -f sdkconfig sdkconfig.old
    idf.py set-target esp32c6
}

reset_bridge_hint() {
    cat <<'EOF'

If synchronization fails:
  1. Keep C6_IO9 connected to GND.
  2. Press the ESP32-P4 RESET button once.
  3. Wait one second.
  4. Run the command again.

Restarting the bridge toggles GPIO54 LOW -> HIGH, which resets the C6 into ROM
download mode while C6_IO9 is held LOW.
EOF
}

probe() {
    require_port

    echo "============================================================"
    echo " C6 probe THROUGH ESP32-P4 UART bridge"
    echo "============================================================"
    echo "Port : $PORT"
    echo "Baud : 115200"
    echo

    python -m esptool \
        --chip esp32c6 \
        -p "$PORT" \
        -b 115200 \
        --before no_reset \
        --after no_reset \
        chip_id

    echo
    python -m esptool \
        --chip esp32c6 \
        -p "$PORT" \
        -b 115200 \
        --before no_reset \
        --after no_reset \
        flash_id

    echo
    echo "=== C6 security information ==="
    python -m esptool \
        --chip esp32c6 \
        -p "$PORT" \
        -b 115200 \
        --before no_reset \
        --after no_reset \
        get_security_info || true

    reset_bridge_hint
}

detect_flash_size_bytes() {
    local output
    output="$(
        python -m esptool \
            --chip esp32c6 \
            -p "$PORT" \
            -b 115200 \
            --before no_reset \
            --after no_reset \
            flash_id 2>&1
    )"

    printf '%s\n' "$output" >&2

    local detected
    detected="$(
        printf '%s\n' "$output" |
        sed -nE 's/.*Detected flash size:[[:space:]]*([0-9]+)(MB|KB).*/\1 \2/p' |
        tail -1
    )"

    [[ -n "$detected" ]] || {
        echo "[ERROR] Could not determine C6 flash size." >&2
        echo "Set C6_FLASH_SIZE_MB manually if required." >&2
        return 1
    }

    local number unit
    read -r number unit <<< "$detected"

    if [[ "$unit" == "MB" ]]; then
        echo $((number * 1024 * 1024))
    else
        echo $((number * 1024))
    fi
}

backup() {
    require_port
    mkdir -p "$BACKUP_DIR"

    local bytes
    if [[ -n "${C6_FLASH_SIZE_MB:-}" ]]; then
        bytes=$((C6_FLASH_SIZE_MB * 1024 * 1024))
    else
        bytes="$(detect_flash_size_bytes)"
    fi

    local stamp out
    stamp="$(date +%Y%m%d_%H%M%S)"
    out="$BACKUP_DIR/c6_factory_full_flash_${stamp}.bin"

    echo
    echo "============================================================"
    echo " BACKING UP COMPLETE C6 FLASH THROUGH P4"
    echo "============================================================"
    echo "Port : $PORT"
    echo "Baud : 115200"
    echo "Bytes: $bytes"
    echo "File : $out"
    echo
    echo "This can take several minutes at 115200 baud."
    echo

    python -m esptool \
        --chip esp32c6 \
        -p "$PORT" \
        -b 115200 \
        --before no_reset \
        --after no_reset \
        read_flash 0x0 "$bytes" "$out"

    echo
    sha256sum "$out"
    echo
    echo "[OK] C6 full-flash backup:"
    echo "     $out"
    echo
    echo "KEEP THIS FILE."
}

build_c6() {
    prepare_c6_target

    echo "============================================================"
    echo " BUILDING C6 SELF-DEEP-SLEEP FIRMWARE"
    echo "============================================================"

    idf.py build

    echo
    echo "[OK] C6 build complete."
}

flash_c6() {
    require_port
    prepare_c6_target

    cd "$C6_PROJECT"

    if [[ ! -f build/flash_args ]]; then
        idf.py build
    fi

    echo "============================================================"
    echo " FLASHING C6 THROUGH ESP32-P4 UART BRIDGE"
    echo "============================================================"
    echo "Port : $PORT"
    echo "Baud : 115200"
    echo

    python -m esptool \
        --chip esp32c6 \
        -p "$PORT" \
        -b 115200 \
        --before no_reset \
        --after no_reset \
        write_flash "@build/flash_args"

    echo
    echo "[OK] C6 self-Deep-sleep firmware written."
    echo
    echo "NEXT:"
    echo "  1. Power OFF."
    echo "  2. Remove C6_IO9 -> GND."
    echo "  3. Remove GPIO20 -> C6_U0RXD."
    echo "  4. Remove GPIO21 -> C6_U0TXD."
    echo "  5. Power ON."
    echo "  6. Flash the normal ESP32-P4 application again."
}

restore_c6() {
    require_port
    local backup_file="${3:-}"

    [[ -n "$backup_file" && -f "$backup_file" ]] || {
        echo "[ERROR] Provide a valid C6 full-flash backup file."
        exit 2
    }

    python -m esptool \
        --chip esp32c6 \
        -p "$PORT" \
        -b 115200 \
        --before no_reset \
        --after no_reset \
        write_flash 0x0 "$backup_file"

    echo "[OK] Raw C6 backup restored."
}

require_tools

case "$ACTION" in
    probe)   probe ;;
    backup)  backup ;;
    build)   build_c6 ;;
    flash)   flash_c6 ;;
    restore) restore_c6 "$@" ;;
    *)
        usage
        exit 2
        ;;
esac
