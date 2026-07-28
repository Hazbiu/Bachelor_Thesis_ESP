#!/usr/bin/env bash
set -euo pipefail

# Folder where this script is located
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

# Project source folders
SRC_DIR="$SCRIPT_DIR/../src"
MAIN_DIR="$SRC_DIR/main"

# Output files beside this script
MAIN_OUT="$SCRIPT_DIR/main_folder_content.txt"
CMAKE_OUT="$SCRIPT_DIR/cmake_files_content.txt"

# Validate folders
if [[ ! -d "$SRC_DIR" ]]; then
    echo "Error: Source folder not found: $SRC_DIR"
    exit 1
fi

if [[ ! -d "$MAIN_DIR" ]]; then
    echo "Error: Main folder not found: $MAIN_DIR"
    exit 1
fi

# Remove old output files
rm -f "$MAIN_OUT" "$CMAKE_OUT"

###############################################################################
# Export all files from src/main
###############################################################################

{
    echo "Main folder export"
    echo "Generated on: $(date)"
    echo "Source: $MAIN_DIR"
    echo "=================================================="
} > "$MAIN_OUT"

main_file_count=0

while IFS= read -r -d '' file; do
    relative_path="${file#$SRC_DIR/}"

    {
        echo
        echo "=================================================="
        echo "FILE: src/$relative_path"
        echo "=================================================="
    } >> "$MAIN_OUT"

    # Include text files and mark binary files
    if grep -Iq . "$file"; then
        cat -- "$file" >> "$MAIN_OUT"
    else
        echo "[Skipped binary/non-text file]" >> "$MAIN_OUT"
    fi

    # Ensure the next file header starts on a new line
    echo >> "$MAIN_OUT"

    ((main_file_count += 1))

done < <(
    find "$MAIN_DIR" -type f -print0 |
        sort -z
)

if ((main_file_count == 0)); then
    echo "[No files found in src/main]" >> "$MAIN_OUT"
fi

###############################################################################
# Export all CMake files from src
#
# Included:
#   - CMakeLists.txt
#   - *.cmake
#
# Excluded directories:
#   - src/managed_components
#   - src/build
###############################################################################

{
    echo "CMake files export"
    echo "Generated on: $(date)"
    echo "Source: $SRC_DIR"
    echo "Included: CMakeLists.txt and *.cmake"
    echo "Excluded: src/managed_components"
    echo "Excluded: src/build"
    echo "=================================================="
} > "$CMAKE_OUT"

cmake_file_count=0

while IFS= read -r -d '' file; do
    relative_path="${file#$SRC_DIR/}"

    {
        echo
        echo "=================================================="
        echo "FILE: src/$relative_path"
        echo "=================================================="
        cat -- "$file"
        echo
    } >> "$CMAKE_OUT"

    ((cmake_file_count += 1))

done < <(
    find "$SRC_DIR" \
        \( \
            -path "$SRC_DIR/managed_components" \
            -o -path "$SRC_DIR/build" \
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
    echo "[No CMake files found]" >> "$CMAKE_OUT"
fi

###############################################################################
# Result
###############################################################################

echo "Done."
echo "Created: $MAIN_OUT"
echo "Main files exported: $main_file_count"
echo "Created: $CMAKE_OUT"
echo "CMake files exported: $cmake_file_count"