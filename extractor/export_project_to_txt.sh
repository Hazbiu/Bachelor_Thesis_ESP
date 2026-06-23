#!/usr/bin/env bash

OUT="project_content_without_managed_components.txt"

# Remove old output file
rm -f "$OUT"

echo "Project export without managed_components" > "$OUT"
echo "Generated on: $(date)" >> "$OUT"
echo "Root: $(pwd)" >> "$OUT"
echo "==================================================" >> "$OUT"
echo "" >> "$OUT"

find . \
  -path "./managed_components" -prune -o \
  -path "./build" -prune -o \
  -path "./.git" -prune -o \
  -path "./dependencies.lock" -prune -o \
  -type f \
  -print | sort | while read -r file; do

    echo "" >> "$OUT"
    echo "==================================================" >> "$OUT"
    echo "FILE: $file" >> "$OUT"
    echo "==================================================" >> "$OUT"

    if file "$file" | grep -q "text"; then
        cat "$file" >> "$OUT"
    else
        echo "[Skipped binary/non-text file]" >> "$OUT"
    fi

done

echo "Done. Created: $OUT"
