#!/usr/bin/env bash

PORT="${1:-/dev/ttyACM0}"
BAUD="${2:-115200}"
PROJECT_DIR="$HOME/Programming/Bachelor_Thesis_ESP/src"
IDF_DIR="$HOME/.espressif/v5.5.4/esp-idf"

LOG_DIR="$PROJECT_DIR/serial_logs"
LOG_FILE="$LOG_DIR/esp32p4_$(date +%Y%m%d_%H%M%S).log"

mkdir -p "$LOG_DIR"

cd "$PROJECT_DIR" || exit 1

if [ -f "$IDF_DIR/export.sh" ]; then
    # Load ESP-IDF tools so idf.py works
    source "$IDF_DIR/export.sh" >/dev/null
else
    echo "ERROR: ESP-IDF export.sh not found at: $IDF_DIR/export.sh"
    exit 1
fi

echo "Opening ESP32-P4 serial monitor"
echo "Port: $PORT"
echo "Baud: $BAUD"
echo "Saving log to: $LOG_FILE"
echo ""
echo "Exit monitor with: Ctrl + ]"
echo ""

idf.py -p "$PORT" -b "$BAUD" monitor 2>&1 | tee "$LOG_FILE"
