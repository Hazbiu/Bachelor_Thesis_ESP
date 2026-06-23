#!/usr/bin/env bash
set -euo pipefail

# Folder where this script is located
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

# Target folder: ../src/main relative to extractor/
TARGET_DIR="$SCRIPT_DIR/../src/main"

# Output file beside this script
OUT="$SCRIPT_DIR/main_folder_content.txt"

# Check target exists
if [[ ! -d "$TARGET_DIR" ]]; then
    echo "Error: Target folder not found: $TARGET_DIR"
    exit 1
fi

# Remove old export
rm -f "$OUT"

{
    echo "Main folder export"
    echo "Generated on: $(date)"
    echo "Source: $TARGET_DIR"
    echo "=================================================="
} > "$OUT"

# Export every file in src/main, including its relative path
while IFS= read -r -d '' file; do
    relative_path="${file#$TARGET_DIR/}"

    {
        echo
        echo "=================================================="
        echo "FILE: src/main/$relative_path"
        echo "=================================================="
    } >> "$OUT"

    # Include text files; mark binaries instead of dumping them
    if grep -Iq . "$file"; then
        cat -- "$file" >> "$OUT"
    else
        echo "[Skipped binary/non-text file]" >> "$OUT"
    fi
done < <(find "$TARGET_DIR" -type f -print0 | sort -z)

echo "Done. Created: $OUT"