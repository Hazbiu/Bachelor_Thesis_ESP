#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd -- "${SCRIPT_DIR}/.." && pwd)"

SRC_DIR="${PROJECT_DIR}/src"
MAIN_DIR="${SRC_DIR}/main"
C6_DIR="${PROJECT_DIR}/c6_deep_sleep_firmware"
FLASHER_DIR="${PROJECT_DIR}/p4_c6_direct_flasher"
UTILITIES_DIR="${PROJECT_DIR}/utilities"

OUTPUT_FILE="${SCRIPT_DIR}/all_project_content.txt"

###############################################################################
# Validate required directories
###############################################################################

for dir in \
    "${MAIN_DIR}" \
    "${C6_DIR}" \
    "${FLASHER_DIR}" \
    "${UTILITIES_DIR}"
do
    if [[ ! -d "${dir}" ]]; then
        echo "ERROR: Missing directory: ${dir}" >&2
        exit 1
    fi
done

###############################################################################
# Helper: reject generated / obsolete / backup files
###############################################################################

is_allowed_file()
{
    local file="$1"

    case "${file}" in
        */build/*)
            return 1
            ;;
        */managed_components/*)
            return 1
            ;;
        */.git/*)
            return 1
            ;;
        *.backup_before_*)
            return 1
            ;;
        *.before_v*)
            return 1
            ;;
        *~)
            return 1
            ;;
    esac

    return 0
}

###############################################################################
# Collect files
###############################################################################

declare -a project_files=()

add_file()
{
    local file="$1"

    if [[ -f "${file}" ]] && is_allowed_file "${file}"; then
        project_files+=("${file}")
    fi
}

###############################################################################
# 1. NORMAL ESP32-P4 APPLICATION
###############################################################################

while IFS= read -r -d '' file; do
    add_file "${file}"
done < <(
    find "${MAIN_DIR}" \
        -type f \
        -print0
)

###############################################################################
# 2. ESP32-P4 PROJECT / CMAKE CONFIGURATION
###############################################################################

while IFS= read -r -d '' file; do
    add_file "${file}"
done < <(
    find "${SRC_DIR}" \
        \( \
            -path "${SRC_DIR}/build" \
            -o -path "${SRC_DIR}/build/*" \
            -o -path "${SRC_DIR}/managed_components" \
            -o -path "${SRC_DIR}/managed_components/*" \
        \) -prune \
        -o \
        -type f \
        \( \
            -name "CMakeLists.txt" \
            -o -name "*.cmake" \
            -o -name "sdkconfig.defaults" \
        \) \
        -print0
)

###############################################################################
# 3. ESP32-C6 SELF-DEEP-SLEEP FIRMWARE
###############################################################################

while IFS= read -r -d '' file; do
    add_file "${file}"
done < <(
    find "${C6_DIR}" \
        \( \
            -path "${C6_DIR}/build" \
            -o -path "${C6_DIR}/build/*" \
            -o -path "${C6_DIR}/managed_components" \
            -o -path "${C6_DIR}/managed_components/*" \
            -o -path "${C6_DIR}/.git" \
            -o -path "${C6_DIR}/.git/*" \
        \) -prune \
        -o \
        -type f \
        -print0
)

###############################################################################
# 4. TEMPORARY P4 -> C6 DIRECT FLASHER FIRMWARE
#
# This contains the code used to:
#   - connect P4 UART to C6 ROM
#   - inspect C6 security state
#   - backup the complete C6 flash to microSD
#   - flash the C6 self-Deep-sleep firmware
#   - MD5 verify the write
#   - restore factory firmware if requested
###############################################################################

while IFS= read -r -d '' file; do
    add_file "${file}"
done < <(
    find "${FLASHER_DIR}" \
        \( \
            -path "${FLASHER_DIR}/build" \
            -o -path "${FLASHER_DIR}/build/*" \
            -o -path "${FLASHER_DIR}/managed_components" \
            -o -path "${FLASHER_DIR}/managed_components/*" \
            -o -path "${FLASHER_DIR}/.git" \
            -o -path "${FLASHER_DIR}/.git/*" \
        \) -prune \
        -o \
        -type f \
        -print0
)

###############################################################################
# 5. ACTIVE FLASHING / DEBUG / MEASUREMENT UTILITIES
###############################################################################

while IFS= read -r -d '' file; do
    add_file "${file}"
done < <(
    find "${UTILITIES_DIR}" \
        -maxdepth 1 \
        -type f \
        \( \
            -name "*.sh" \
            -o -name "*.py" \
        \) \
        -print0
)

###############################################################################
# Sort and remove duplicates
###############################################################################

mapfile -d '' sorted_files < <(
    printf '%s\0' "${project_files[@]}" |
    sort -zu
)

project_files=("${sorted_files[@]}")

if (( ${#project_files[@]} == 0 )); then
    echo "ERROR: No files found." >&2
    exit 1
fi

###############################################################################
# Count subsystems
###############################################################################

p4_count=0
c6_count=0
flasher_count=0
utility_count=0
other_count=0

for file in "${project_files[@]}"; do
    case "${file}" in
        "${MAIN_DIR}"/*)
            ((p4_count += 1))
            ;;
        "${C6_DIR}"/*)
            ((c6_count += 1))
            ;;
        "${FLASHER_DIR}"/*)
            ((flasher_count += 1))
            ;;
        "${UTILITIES_DIR}"/*)
            ((utility_count += 1))
            ;;
        *)
            ((other_count += 1))
            ;;
    esac
done

###############################################################################
# Write header
###############################################################################

{
    echo "COMPLETE ESP32-P4 + ESP32-C6 PROJECT EXPORT"
    echo "Generated on: $(date)"
    echo "Project: ${PROJECT_DIR}"
    echo
    echo "=================================================="
    echo "INCLUDED FIRMWARE"
    echo "=================================================="
    echo
    echo "1. ESP32-P4 MAIN APPLICATION"
    echo "   src/main/"
    echo
    echo "2. ESP32-C6 SELF-DEEP-SLEEP FIRMWARE"
    echo "   c6_deep_sleep_firmware/"
    echo
    echo "3. ESP32-P4 -> ESP32-C6 DIRECT FLASHER"
    echo "   p4_c6_direct_flasher/"
    echo
    echo "4. FLASHING / DEBUG UTILITIES"
    echo "   utilities/"
    echo
    echo "Excluded:"
    echo "   build/"
    echo "   managed_components/"
    echo "   .git/"
    echo "   *.backup_before_*"
    echo "   *.before_v*"
    echo
    echo "=================================================="
    echo "FILE COUNTS"
    echo "=================================================="
    echo
    echo "P4 main application : ${p4_count}"
    echo "C6 firmware         : ${c6_count}"
    echo "P4-C6 direct flasher: ${flasher_count}"
    echo "Utilities           : ${utility_count}"
    echo "Other config files  : ${other_count}"
    echo "Total               : ${#project_files[@]}"
    echo
    echo "=================================================="
    echo "DEEP-SLEEP ARCHITECTURE"
    echo "=================================================="
    echo
    echo "ESP32-P4 main firmware"
    echo "        |"
    echo "        | enters Deep-sleep"
    echo "        v"
    echo "GPIO54 / C6 CHIP_PU changes state"
    echo "        |"
    echo "        v"
    echo "ESP32-C6 boots"
    echo "        |"
    echo "        v"
    echo "C6 self-Deep-sleep firmware"
    echo "        |"
    echo "        v"
    echo "esp_deep_sleep_start()"
    echo
    echo "=================================================="
    echo "C6 FLASHING ARCHITECTURE"
    echo "=================================================="
    echo
    echo "Laptop USB"
    echo "    |"
    echo "    v"
    echo "ESP32-P4 temporary direct flasher"
    echo "    |"
    echo "    +--> GPIO20 -> C6 UART RX"
    echo "    +--> GPIO21 <- C6 UART TX"
    echo "    +--> GPIO54 -> C6 CHIP_PU"
    echo "    |"
    echo "    v"
    echo "ESP32-C6 ROM / flasher stub"
    echo
    echo "=================================================="
    echo "FILE MANIFEST"
    echo "=================================================="

    for file in "${project_files[@]}"; do
        echo "${file#"${PROJECT_DIR}/"}"
    done

    echo
    echo "=================================================="
    echo "FILE CONTENTS"
    echo "=================================================="

} > "${OUTPUT_FILE}"

###############################################################################
# Append contents
###############################################################################

exported_count=0
binary_count=0

for file in "${project_files[@]}"; do
    relative="${file#"${PROJECT_DIR}/"}"

    {
        echo
        echo "=================================================="
        echo "FILE: ${relative}"
        echo "=================================================="

        if [[ ! -s "${file}" ]]; then
            echo "[Empty file]"

        elif LC_ALL=C grep -Iq . -- "${file}"; then
            cat -- "${file}"

        else
            echo "[Binary/non-text file skipped]"
            ((binary_count += 1))
        fi

        echo
    } >> "${OUTPUT_FILE}"

    ((exported_count += 1))
done

###############################################################################
# Final verification
###############################################################################

required_export_entries=(
    "src/main/power/app_sleep.c"
    "src/main/power/deep_sleep.c"
    "src/main/power/wake_up.c"
    "c6_deep_sleep_firmware/main/main.c"
    "p4_c6_direct_flasher/main/main.c"
    "utilities/esp_AI_flashing.sh"
    "utilities/esp_P4_C6_direct.sh"
)

for required in "${required_export_entries[@]}"; do
    if ! grep -Fqx "FILE: ${required}" "${OUTPUT_FILE}"; then
        echo
        echo "ERROR: Required file was not exported:"
        echo "  ${required}"
        exit 1
    fi
done

echo
echo "============================================================"
echo " COMPLETE EXPORT FINISHED"
echo "============================================================"
echo
echo "Created:"
echo "  ${OUTPUT_FILE}"
echo
echo "ESP32-P4 files:"
echo "  ${p4_count}"
echo
echo "ESP32-C6 firmware files:"
echo "  ${c6_count}"
echo
echo "P4 -> C6 direct-flasher files:"
echo "  ${flasher_count}"
echo
echo "Utility scripts:"
echo "  ${utility_count}"
echo
echo "Total exported file sections:"
echo "  ${exported_count}"
echo
echo "Binary files skipped:"
echo "  ${binary_count}"
echo
echo "Critical P4 + C6 + flashing files:"
echo "  VERIFIED"
echo
