#!/usr/bin/env bash
#
# Deep-sleep fix installer for Bachelor_Thesis_ESP.
#
# Usage:
#   ./install.sh [path/to/src/main]
#
# With no argument the script looks for ./src/main, then ../src/main, then the
# current directory. Every file it is about to overwrite is copied into a
# timestamped backup folder first, so the change is always reversible.
#
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
SOURCE_DIR="${SCRIPT_DIR}/main"

RED=$'\033[0;31m'; GREEN=$'\033[0;32m'; YELLOW=$'\033[0;33m'
BLUE=$'\033[0;34m'; BOLD=$'\033[1m'; RESET=$'\033[0m'

die() { printf '%sERROR:%s %s\n' "$RED" "$RESET" "$1" >&2; exit 1; }

# Files this package installs, relative to src/main.
FILES=(
    "CMakeLists.txt"
    "diagnostics/app_logging.c"
    "include/config/app_config.h"
    "include/power_save/component_display.h"
    "power/app_sleep.c"
    "power/component_audio.c"
    "power/component_display.c"
    "power/component_ethernet.c"
    "power/component_sdcard.c"
    "power/component_wifi.c"
    "power/deep_sleep.c"
)

# ---------------------------------------------------------------------------
# Locate the target directory
# ---------------------------------------------------------------------------
looks_like_project_main() {
    [[ -f "$1/app/app_main.c" && -f "$1/power/deep_sleep.c" ]]
}

if [[ $# -ge 1 ]]; then
    TARGET_DIR="$1"
    looks_like_project_main "$TARGET_DIR" \
        || die "'$TARGET_DIR' does not look like src/main (no app/app_main.c + power/deep_sleep.c)."
else
    TARGET_DIR=""
    for candidate in "./src/main" "../src/main" "."; do
        if looks_like_project_main "$candidate"; then
            TARGET_DIR="$candidate"
            break
        fi
    done
    [[ -n "$TARGET_DIR" ]] \
        || die "Could not find src/main. Run: $0 /path/to/Bachelor_Thesis_ESP/src/main"
fi

TARGET_DIR="$(cd -- "$TARGET_DIR" && pwd)"
[[ -d "$SOURCE_DIR" ]] || die "Package payload missing: $SOURCE_DIR"

printf '%s%sDeep-sleep fix installer%s\n' "$BOLD" "$BLUE" "$RESET"
printf '  target : %s\n' "$TARGET_DIR"

# ---------------------------------------------------------------------------
# Verify the payload is complete before touching anything
# ---------------------------------------------------------------------------
for rel in "${FILES[@]}"; do
    [[ -f "${SOURCE_DIR}/${rel}" ]] || die "Package is incomplete, missing: main/${rel}"
done

# ---------------------------------------------------------------------------
# Back up every file that will be replaced
# ---------------------------------------------------------------------------
STAMP="$(date +%Y%m%d_%H%M%S)"
BACKUP_DIR="${TARGET_DIR}/../../deep_sleep_backup_${STAMP}"
mkdir -p "$BACKUP_DIR"
BACKUP_DIR="$(cd -- "$BACKUP_DIR" && pwd)"
printf '  backup : %s\n\n' "$BACKUP_DIR"

replaced=0
created=0

for rel in "${FILES[@]}"; do
    dest="${TARGET_DIR}/${rel}"

    if [[ -f "$dest" ]]; then
        mkdir -p "${BACKUP_DIR}/$(dirname -- "$rel")"
        cp -p -- "$dest" "${BACKUP_DIR}/${rel}"
        printf '  %sreplace%s  %s\n' "$YELLOW" "$RESET" "$rel"
        replaced=$((replaced + 1))
    else
        printf '  %snew%s      %s\n' "$GREEN" "$RESET" "$rel"
        created=$((created + 1))
    fi

    mkdir -p "$(dirname -- "$dest")"
    cp -- "${SOURCE_DIR}/${rel}" "$dest"
done

if [[ $replaced -eq 0 ]]; then
    rmdir -- "$BACKUP_DIR" 2>/dev/null || true
fi

printf '\n%sDone:%s %d replaced, %d new.\n' "$BOLD" "$RESET" "$replaced" "$created"

# ---------------------------------------------------------------------------
# Post-install reminders
# ---------------------------------------------------------------------------
CONFIG="${TARGET_DIR}/include/config/app_config.h"

if grep -qE '^#define APP_PWR_(DISPLAY_BACKLIGHT|TOUCH_INT|TOUCH_RESET)_GPIO[[:space:]]+\(-1\)' "$CONFIG" 2>/dev/null; then
    printf '\n%sBefore flashing%s - three pins are still placeholders in\n' "$BOLD$YELLOW" "$RESET"
    printf '  include/config/app_config.h\n\n'
    grep -nE '^#define APP_PWR_(DISPLAY_BACKLIGHT|TOUCH_INT|TOUCH_RESET)_GPIO' "$CONFIG" \
        | sed 's/^/    /'
    printf '\n  Fill these in from the Waveshare schematic. The GT911 I2C sleep\n'
    printf '  command works without them, but a floating backlight-enable pad can\n'
    printf '  leave the driver partly on and a floating INT can wake the GT911\n'
    printf '  straight back out of sleep.\n'
fi

if git -C "$TARGET_DIR" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    printf '\n  Review the change:  git -C %s diff\n' "$(git -C "$TARGET_DIR" rev-parse --show-toplevel)"
fi

if [[ $replaced -gt 0 ]]; then
    printf '  Undo everything:    cp -a %s/. %s/\n' "$BACKUP_DIR" "$TARGET_DIR"
fi

printf '\n  Then rebuild:       idf.py build flash monitor\n\n'
