#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BRIDGE_PROJECT="$ROOT/p4_c6_uart_bridge"

ACTION="${1:-}"
PORT="${2:-/dev/ttyACM0}"

usage() {
    cat <<'EOF'
Temporary ESP32-P4 -> ESP32-C6 UART bridge helper

Usage:
  ./utilities/esp_P4_C6_bridge.sh build
  ./utilities/esp_P4_C6_bridge.sh flash /dev/ttyACM0

After flash succeeds:
  - keep C6_IO9 -> GND
  - keep GPIO20 -> C6_U0RXD
  - keep GPIO21 -> C6_U0TXD
  - wait one second
  - use esp_C6_sleep_flashing.sh on the SAME /dev/ttyACM0

Do NOT run idf.py monitor while the raw bridge is active.
EOF
}

require_idf() {
    command -v idf.py >/dev/null 2>&1 || {
        echo "[ERROR] idf.py not found."
        echo "Activate ESP-IDF, e.g.:"
        echo "  source ~/.espressif/tools/activate_idf_v5.5.4.sh"
        exit 1
    }
}

prepare_target() {
    cd "$BRIDGE_PROJECT"

    # This board is ESP32-P4 revision v1.3. ESP-IDF 5.5 defaults a fresh
    # ESP32-P4 project to minimum revision v3.1, so every bridge build must
    # explicitly select the pre-v3 silicon compatibility path.
    if [[ -f sdkconfig ]] &&
       grep -q 'CONFIG_IDF_TARGET="esp32p4"' sdkconfig &&
       grep -q '^CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y$' sdkconfig; then
        return
    fi

    echo "[INFO] Regenerating bridge for ESP32-P4 revision < v3..."
    rm -rf build
    rm -f sdkconfig sdkconfig.old

    idf.py set-target esp32p4

    if ! grep -q '^CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y$' sdkconfig; then
        echo "[ERROR] ESP32-P4 pre-v3 compatibility was not selected."
        echo "Expected CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y"
        exit 1
    fi

    echo "[OK] Bridge configured for ESP32-P4 revisions < v3."
}

build_bridge() {
    prepare_target

    echo "============================================================"
    echo " BUILDING TEMPORARY P4 -> C6 UART BRIDGE"
    echo "============================================================"
    echo "USB/P4 UART0 : GPIO37 TX / GPIO38 RX"
    echo "P4/C6 UART1  : GPIO20 TX / GPIO21 RX"
    echo "C6 reset      : GPIO54 -> CHIP_PU"
    echo

    idf.py build
}

flash_bridge() {
    [[ -e "$PORT" ]] || {
        echo "[ERROR] Port does not exist: $PORT"
        exit 1
    }

    prepare_target

    if [[ ! -f "$BRIDGE_PROJECT/build/p4_c6_uart_bridge.bin" ]]; then
        idf.py build
    fi

    echo "============================================================"
    echo " FLASHING TEMPORARY P4 UART BRIDGE"
    echo "============================================================"
    echo "Port: $PORT"
    echo
    echo "Verify these remain connected:"
    echo "  C6_IO9 -> GND"
    echo "  GPIO20 -> C6_U0RXD"
    echo "  GPIO21 -> C6_U0TXD"
    echo
    echo "P4 revision configuration:"
    grep -E '^CONFIG_ESP32P4_SELECTS_REV_LESS_V3=|^CONFIG_ESP32P4_REV_MIN_' \
        "$BRIDGE_PROJECT/sdkconfig" || true
    echo
    echo "The bridge was rebuilt for the v1.3 / pre-v3 P4 path."
    echo

    cd "$BRIDGE_PROJECT"
    idf.py -p "$PORT" flash

    echo
    echo "============================================================"
    echo " P4 UART bridge installed"
    echo "============================================================"
    echo
    echo "Wait 1 second. Do NOT start a serial monitor."
    echo
    echo "Next:"
    echo "  cd \"$ROOT\""
    echo "  ./utilities/esp_C6_sleep_flashing.sh probe $PORT"
}

require_idf

case "$ACTION" in
    build) build_bridge ;;
    flash) flash_bridge ;;
    *)
        usage
        exit 2
        ;;
esac
