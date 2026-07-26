#!/usr/bin/env bash

set -Eeuo pipefail

# Always operate from the directory containing this script.
PROJECT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
cd "$PROJECT_DIR"

if [[ ! -f "CMakeLists.txt" ]]; then
    echo "Error: CMakeLists.txt was not found in:"
    echo "  $PROJECT_DIR"
    exit 1
fi

# Load ESP-IDF automatically when idf.py is not already available.
if ! command -v idf.py >/dev/null 2>&1; then
    IDF_EXPORT="$(
        find "$HOME/.espressif" \
            -type f \
            -path '*/esp-idf/export.sh' \
            2>/dev/null |
        sort -V |
        tail -n 1
    )"

    if [[ -z "$IDF_EXPORT" ]]; then
        echo "Error: idf.py is unavailable and export.sh was not found."
        exit 1
    fi

    echo "Loading ESP-IDF from:"
    echo "  $IDF_EXPORT"
    source "$IDF_EXPORT" >/dev/null
fi

# ESPPORT can be provided manually:
# ESPPORT=/dev/ttyACM1 ./smart_flash.sh
PORT="${ESPPORT:-}"

if [[ -z "$PORT" ]]; then
    shopt -s nullglob
    PORTS=(/dev/ttyACM* /dev/ttyUSB*)
    shopt -u nullglob

    case "${#PORTS[@]}" in
        0)
            echo "Error: no ESP32 serial port was found."
            echo "Check the USB cable and board connection."
            exit 1
            ;;
        1)
            PORT="${PORTS[0]}"
            ;;
        *)
            echo "Error: multiple serial ports were found:"
            printf '  %s\n' "${PORTS[@]}"
            echo
            echo "Choose one using:"
            echo "  ESPPORT=/dev/ttyACM0 ./smart_flash.sh"
            exit 1
            ;;
    esac
fi

echo "Project: $PROJECT_DIR"
echo "Port:    $PORT"
echo "Running incremental build and flashing..."

case "${1:-flash}" in
    flash)
        idf.py -p "$PORT" flash
        ;;
    monitor|flash-monitor)
        idf.py -p "$PORT" flash monitor
        ;;
    *)
        echo "Usage:"
        echo "  ./smart_flash.sh          Build changed files and flash"
        echo "  ./smart_flash.sh monitor  Build, flash, and open monitor"
        exit 2
        ;;
esac
