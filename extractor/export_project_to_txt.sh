#!/usr/bin/env bash

set -euo pipefail

# Folder containing this script: Bachelor_Thesis_ESP/extractor
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

# Project folders
PROJECT_DIR="$(cd -- "${SCRIPT_DIR}/.." && pwd)"
SRC_DIR="${PROJECT_DIR}/src"
MAIN_DIR="${SRC_DIR}/main"
SLEEP_DEEP_DIR="${MAIN_DIR}/features/sleep_deep"

# Output files beside this script
MAIN_OUT="${SCRIPT_DIR}/main_folder_content.txt"
CMAKE_OUT="${SCRIPT_DIR}/cmake_files_content.txt"
MAIN_MANIFEST_OUT="${SCRIPT_DIR}/main_files_manifest.txt"

if [[ ! -d "${SRC_DIR}" ]]; then
    echo "Error: Source folder not found: ${SRC_DIR}" >&2
    exit 1
fi

if [[ ! -d "${MAIN_DIR}" ]]; then
    echo "Error: Main folder not found: ${MAIN_DIR}" >&2
    exit 1
fi

###############################################################################
# Verify the reorganized deep-sleep feature
###############################################################################

required_sleep_files=(
    "${SLEEP_DEEP_DIR}/app_sleep.c"
    "${SLEEP_DEEP_DIR}/app_sleep.h"
    "${SLEEP_DEEP_DIR}/deep_sleep.c"
    "${SLEEP_DEEP_DIR}/deep_sleep.h"
    "${SLEEP_DEEP_DIR}/wake_up.c"
    "${SLEEP_DEEP_DIR}/wake_up.h"
)

missing_sleep_files=0

echo "Checking deep-sleep files:"

for file in "${required_sleep_files[@]}"; do
    relative_path="${file#"${PROJECT_DIR}/"}"

    if [[ -f "${file}" ]]; then
        echo "  [FOUND] ${relative_path}"
    else
        echo "  [MISSING] ${relative_path}" >&2
        ((missing_sleep_files += 1))
    fi
done

if ((missing_sleep_files > 0)); then
    echo "Error: ${missing_sleep_files} required deep-sleep file(s) are missing." >&2
    echo "Apply the deep-sleep reorganization update before exporting." >&2
    exit 1
fi

###############################################################################
# Helper: append one complete file to an export
###############################################################################

append_file_to_export() {
    local output_file="$1"
    local source_file="$2"
    local relative_path

    relative_path="${source_file#"${SRC_DIR}/"}"

    {
        echo
        echo "=================================================="
        echo "FILE: src/${relative_path}"
        echo "=================================================="

        if [[ ! -s "${source_file}" ]]; then
            echo "[Empty file]"
        elif LC_ALL=C grep -Iq . -- "${source_file}"; then
            cat -- "${source_file}"
        else
            echo "[Skipped binary/non-text file]"
        fi

        # Ensure the next file header always starts on a new line.
        echo
    } >> "${output_file}"
}

###############################################################################
# Export every file below src/main recursively
###############################################################################

{
    echo "Main folder export"
    echo "Generated on: $(date)"
    echo "Source: ${MAIN_DIR}"
    echo "Included: every regular file below src/main recursively"
    echo "=================================================="
} > "${MAIN_OUT}"

{
    echo "Main files manifest"
    echo "Generated on: $(date)"
    echo "Source: ${MAIN_DIR}"
    echo "=================================================="
} > "${MAIN_MANIFEST_OUT}"

main_file_count=0

while IFS= read -r -d '' file; do
    relative_path="${file#"${SRC_DIR}/"}"

    printf 'src/%s\n' "${relative_path}" >> "${MAIN_MANIFEST_OUT}"
    append_file_to_export "${MAIN_OUT}" "${file}"

    ((main_file_count += 1))
done < <(
    find "${MAIN_DIR}" -type f -print0 | sort -z
)

if ((main_file_count == 0)); then
    echo "[No files found in src/main]" >> "${MAIN_OUT}"
fi

###############################################################################
# Confirm that every reorganized deep-sleep file reached the export
###############################################################################

exported_sleep_file_count=0

for file in "${required_sleep_files[@]}"; do
    relative_path="${file#"${SRC_DIR}/"}"
    expected_header="FILE: src/${relative_path}"

    if grep -Fqx -- "${expected_header}" "${MAIN_OUT}"; then
        ((exported_sleep_file_count += 1))
    else
        echo "Error: File was not exported: src/${relative_path}" >&2
        exit 1
    fi
done

###############################################################################
# Export every CMakeLists.txt and *.cmake file below src
#
# Excluded:
#   - src/managed_components
#   - src/build
###############################################################################

{
    echo "CMake files export"
    echo "Generated on: $(date)"
    echo "Source: ${SRC_DIR}"
    echo "Included: CMakeLists.txt and *.cmake"
    echo "Excluded: src/managed_components"
    echo "Excluded: src/build"
    echo "=================================================="
} > "${CMAKE_OUT}"

cmake_file_count=0

while IFS= read -r -d '' file; do
    append_file_to_export "${CMAKE_OUT}" "${file}"
    ((cmake_file_count += 1))
done < <(
    find "${SRC_DIR}" \
        \( \
            -path "${SRC_DIR}/managed_components" \
            -o -path "${SRC_DIR}/build" \
        \) -prune \
        -o \
        -type f \
        \( \
            -name "CMakeLists.txt" \
            -o -name "*.cmake" \
        \) \
        -print0 |
        sort -z
)

if ((cmake_file_count == 0)); then
    echo "[No CMake files found]" >> "${CMAKE_OUT}"
fi

###############################################################################
# Result
###############################################################################

echo
echo "Done."
echo "Created: ${MAIN_OUT}"
echo "Main files exported: ${main_file_count}"
echo "Deep-sleep files exported: ${exported_sleep_file_count}/6"
echo "Created: ${MAIN_MANIFEST_OUT}"
echo "Created: ${CMAKE_OUT}"
echo "CMake files exported: ${cmake_file_count}"
