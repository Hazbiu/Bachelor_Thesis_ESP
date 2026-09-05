#!/usr/bin/env bash

set -euo pipefail

###############################################################################
# PATHS
###############################################################################

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd -- "${SCRIPT_DIR}/.." && pwd)"

MAIN_DIR="${PROJECT_DIR}/src/main"
OUTPUT_FILE="${SCRIPT_DIR}/all_project_content.txt"

###############################################################################
# VALIDATE
###############################################################################

if [[ ! -d "${MAIN_DIR}" ]]; then
    echo "ERROR: Main directory does not exist:"
    echo "  ${MAIN_DIR}"
    exit 1
fi

###############################################################################
# START
###############################################################################

echo
echo "============================================================"
echo " ESP32 src/main SOURCE EXPORTER"
echo "============================================================"
echo
echo "Source:"
echo "  ${MAIN_DIR}"
echo
echo "Output:"
echo "  ${OUTPUT_FILE}"
echo

###############################################################################
# FIND FILES ONLY INSIDE src/main
###############################################################################

declare -a FILES=()

echo "Scanning src/main recursively..."

while IFS= read -r -d '' file; do
    FILES+=("${file}")
done < <(
    find "${MAIN_DIR}" \
        -type f \
        \( \
            -name "*.c" \
            -o -name "*.h" \
            -o -name "*.cpp" \
            -o -name "*.hpp" \
            -o -name "*.cc" \
            -o -name "*.hh" \
            -o -name "*.cxx" \
            -o -name "*.hxx" \
            -o -name "*.S" \
            -o -name "*.s" \
            -o -name "*.sh" \
            -o -name "*.py" \
            -o -name "*.cmake" \
            -o -name "CMakeLists.txt" \
            -o -name "*.yml" \
            -o -name "*.yaml" \
            -o -name "*.json" \
            -o -name "*.txt" \
            -o -name "*.md" \
        \) \
        -print0
)

echo "Scan finished."

###############################################################################
# SORT
###############################################################################

if (( ${#FILES[@]} == 0 )); then
    echo
    echo "ERROR: No matching files found inside:"
    echo "  ${MAIN_DIR}"
    exit 1
fi

mapfile -d '' SORTED_FILES < <(
    printf '%s\0' "${FILES[@]}" |
    sort -z
)

FILES=("${SORTED_FILES[@]}")

TOTAL="${#FILES[@]}"

echo "Found ${TOTAL} files."

###############################################################################
# COUNTERS
###############################################################################

C_COUNT=0
H_COUNT=0
CPP_COUNT=0
OTHER_COUNT=0

for file in "${FILES[@]}"; do

    case "${file}" in
        *.c)
            ((C_COUNT += 1))
            ;;

        *.h)
            ((H_COUNT += 1))
            ;;

        *.cpp|*.hpp|*.cc|*.hh|*.cxx|*.hxx)
            ((CPP_COUNT += 1))
            ;;

        *)
            ((OTHER_COUNT += 1))
            ;;
    esac

done

###############################################################################
# HEADER
###############################################################################

cat > "${OUTPUT_FILE}" <<EOF
ESP32-P4 MAIN APPLICATION SOURCE EXPORT

Generated on: $(date)
Project: ${PROJECT_DIR}
Source directory: src/main/

============================================================
FILE COUNTS
============================================================

C files       : ${C_COUNT}
Header files  : ${H_COUNT}
C++ files     : ${CPP_COUNT}
Other files   : ${OTHER_COUNT}
Total files   : ${TOTAL}

============================================================
FILE MANIFEST
============================================================

EOF

###############################################################################
# MANIFEST
###############################################################################

for file in "${FILES[@]}"; do

    relative="${file#"${PROJECT_DIR}/"}"

    echo "${relative}" >> "${OUTPUT_FILE}"

done

cat >> "${OUTPUT_FILE}" <<'EOF'

============================================================
FILE CONTENTS
============================================================
EOF

###############################################################################
# EXPORT CONTENT
###############################################################################

echo "Exporting..."

EXPORTED=0

for file in "${FILES[@]}"; do

    relative="${file#"${PROJECT_DIR}/"}"

    {
        echo
        echo
        echo "============================================================"
        echo "FILE: ${relative}"
        echo "============================================================"
        echo

        if [[ -s "${file}" ]]; then
            cat -- "${file}"
        else
            echo "[EMPTY FILE]"
        fi

    } >> "${OUTPUT_FILE}"

    ((EXPORTED += 1))

done

###############################################################################
# FINISH
###############################################################################

echo
echo "============================================================"
echo " EXPORT FINISHED"
echo "============================================================"
echo
echo "Source:"
echo "  src/main/"
echo
echo "Files found:"
echo "  ${TOTAL}"
echo
echo "Files exported:"
echo "  ${EXPORTED}"
echo
echo "C files:"
echo "  ${C_COUNT}"
echo
echo "Header files:"
echo "  ${H_COUNT}"
echo
echo "C++ files:"
echo "  ${CPP_COUNT}"
echo
echo "Created:"
echo "  ${OUTPUT_FILE}"
echo