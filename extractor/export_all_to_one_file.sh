#!/usr/bin/env bash

set -euo pipefail

# Folder containing this script: Bachelor_Thesis_ESP/extractor
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

# Project folders
PROJECT_DIR="$(cd -- "${SCRIPT_DIR}/.." && pwd)"
SRC_DIR="${PROJECT_DIR}/src"
MAIN_DIR="${SRC_DIR}/main"

# One combined output file beside this script
OUTPUT_FILE="${SCRIPT_DIR}/all_project_content.txt"

if [[ ! -d "${SRC_DIR}" ]]; then
    echo "Error: Source folder not found: ${SRC_DIR}" >&2
    exit 1
fi

if [[ ! -d "${MAIN_DIR}" ]]; then
    echo "Error: Main folder not found: ${MAIN_DIR}" >&2
    exit 1
fi

###############################################################################
# Build one sorted, duplicate-free list containing:
#
#   1. Every regular file under src/main recursively.
#   2. Every CMakeLists.txt and *.cmake file under src recursively.
#
# The generated build tree and managed components are excluded from the CMake
# search. src/main/CMakeLists.txt is found by both searches, but sort -zu keeps
# it only once.
###############################################################################

project_files=()

while IFS= read -r -d '' file; do
    project_files+=("${file}")
done < <(
    {
        find "${MAIN_DIR}" -type f -print0

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
            -print0
    } | sort -zu
)

file_count="${#project_files[@]}"

if ((file_count == 0)); then
    echo "Error: No project files were found." >&2
    exit 1
fi

###############################################################################
# Verify that the complete reorganized deep-sleep feature will be exported.
###############################################################################

required_sleep_files=(
    "${MAIN_DIR}/features/sleep_deep/app_sleep.c"
    "${MAIN_DIR}/features/sleep_deep/app_sleep.h"
    "${MAIN_DIR}/features/sleep_deep/deep_sleep.c"
    "${MAIN_DIR}/features/sleep_deep/deep_sleep.h"
    "${MAIN_DIR}/features/sleep_deep/wake_up.c"
    "${MAIN_DIR}/features/sleep_deep/wake_up.h"
)

for file in "${required_sleep_files[@]}"; do
    if [[ ! -f "${file}" ]]; then
        relative_path="${file#"${PROJECT_DIR}/"}"
        echo "Error: Required deep-sleep file is missing: ${relative_path}" >&2
        exit 1
    fi
done

###############################################################################
# Write the header and complete manifest to the one output file.
###############################################################################

{
    echo "Complete project export"
    echo "Generated on: $(date)"
    echo "Project: ${PROJECT_DIR}"
    echo "Included: every file under src/main"
    echo "Included: every CMakeLists.txt and *.cmake file under src"
    echo "Excluded from CMake search: src/managed_components"
    echo "Excluded from CMake search: src/build"
    echo "Unique files: ${file_count}"
    echo "=================================================="
    echo
    echo "FILE MANIFEST"
    echo "=================================================="

    for file in "${project_files[@]}"; do
        relative_path="${file#"${PROJECT_DIR}/"}"
        echo "${relative_path}"
    done

    echo
    echo "FILE CONTENTS"
    echo "=================================================="
} > "${OUTPUT_FILE}"

###############################################################################
# Append every complete file.
###############################################################################

exported_count=0

for file in "${project_files[@]}"; do
    relative_path="${file#"${PROJECT_DIR}/"}"

    {
        echo
        echo "=================================================="
        echo "FILE: ${relative_path}"
        echo "=================================================="

        if [[ ! -s "${file}" ]]; then
            echo "[Empty file]"
        elif LC_ALL=C grep -Iq . -- "${file}"; then
            cat -- "${file}"
        else
            echo "[Skipped binary/non-text file]"
        fi

        # Ensure the next file header begins on a new line.
        echo
    } >> "${OUTPUT_FILE}"

    ((exported_count += 1))
done

###############################################################################
# Verify the resulting combined file.
###############################################################################

header_count="$(grep -c '^FILE: ' "${OUTPUT_FILE}" || true)"

if [[ "${header_count}" -ne "${exported_count}" ]]; then
    echo "Error: Export verification failed." >&2
    echo "Expected ${exported_count} file sections, found ${header_count}." >&2
    exit 1
fi

for file in "${required_sleep_files[@]}"; do
    relative_path="${file#"${PROJECT_DIR}/"}"

    if ! grep -Fqx -- "FILE: ${relative_path}" "${OUTPUT_FILE}"; then
        echo "Error: Deep-sleep file was not exported: ${relative_path}" >&2
        exit 1
    fi
done

echo "Done."
echo "Created: ${OUTPUT_FILE}"
echo "Unique files exported: ${exported_count}"
echo "Deep-sleep files exported: 6/6"
