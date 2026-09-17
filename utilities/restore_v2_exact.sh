#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd -- "${SCRIPT_DIR}/.." && pwd)"
BSP_FILE="${PROJECT_ROOT}/src_p4/components/waveshare__esp32_p4_platform/esp32_p4_platform.c"
BSP_BACKUP="${BSP_FILE}.before_gt911_startup_hotfix"

# Undo only the V3 BSP hotfix if it was actually applied.
if [[ -f "${BSP_BACKUP}" ]]; then
    cp -f "${BSP_BACKUP}" "${BSP_FILE}"
    rm -f "${BSP_BACKUP}"
    echo "[OK] Restored original Waveshare BSP from the V3 hotfix backup."
else
    echo "[OK] No V3 BSP backup found; BSP left unchanged."
fi

# Remove files that existed only in V3.
rm -f "${PROJECT_ROOT}/utilities/apply_gt911_startup_hotfix.sh"
rm -f "${PROJECT_ROOT}/README_GT911_HOTFIX.txt"

echo "[OK] V3-only files removed."
echo "[OK] Repository source is back on the V2 GUI/power files supplied by this archive."
