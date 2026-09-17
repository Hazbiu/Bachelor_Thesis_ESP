#!/usr/bin/env bash

set -euo pipefail

echo "=========================================="
echo "       Joulescope UI Launcher"
echo "=========================================="
echo

VENV="$HOME/.venvs/joulescope-ui"
PYTHON="$VENV/bin/python"

# ------------------------------------------------------------
# 1. Check Ubuntu dependencies
# ------------------------------------------------------------

PACKAGES=(
    python3
    python3-venv
    python3-pip
    libudev-dev
    libxcb-cursor0
    wget
)

MISSING=()

for pkg in "${PACKAGES[@]}"; do
    if ! dpkg -s "$pkg" >/dev/null 2>&1; then
        MISSING+=("$pkg")
    fi
done

if [ ${#MISSING[@]} -gt 0 ]; then
    echo "Installing missing Ubuntu packages:"
    printf '  %s\n' "${MISSING[@]}"

    sudo apt update
    sudo apt install -y "${MISSING[@]}"
else
    echo "[OK] Ubuntu dependencies already installed."
fi

echo

# ------------------------------------------------------------
# 2. Create dedicated Joulescope virtual environment
# ------------------------------------------------------------

if [ ! -x "$PYTHON" ]; then
    echo "Creating Joulescope Python environment:"
    echo "  $VENV"

    mkdir -p "$(dirname "$VENV")"

    /usr/bin/python3 -m venv "$VENV"

    "$PYTHON" -m pip install --upgrade pip
else
    echo "[OK] Joulescope virtual environment exists."
fi

echo

# ------------------------------------------------------------
# 3. Install Joulescope UI if necessary
# ------------------------------------------------------------

if "$PYTHON" -c "import joulescope_ui" >/dev/null 2>&1; then
    echo "[OK] Joulescope UI is installed."
else
    echo "Installing Joulescope UI..."

    "$PYTHON" -m pip install \
        -U \
        --upgrade-strategy=eager \
        joulescope_ui
fi

echo

# ------------------------------------------------------------
# 4. Install Joulescope USB udev rule
# ------------------------------------------------------------

RULE="/etc/udev/rules.d/99-joulescope.rules"

if [ ! -f "$RULE" ]; then

    echo "Installing Joulescope USB permission rule..."

    TMP_RULE="$(mktemp)"

    wget -q \
        https://raw.githubusercontent.com/jetperch/joulescope_driver/main/99-joulescope.rules \
        -O "$TMP_RULE"

    sudo cp "$TMP_RULE" "$RULE"
    rm -f "$TMP_RULE"

    sudo udevadm control --reload-rules
    sudo udevadm trigger

    echo "[OK] Joulescope USB rule installed."
    echo
    echo "If the Joulescope is already connected,"
    echo "unplug it and plug it back in."
else
    echo "[OK] Joulescope USB permission rule already installed."
fi

echo

# ------------------------------------------------------------
# 5. Launch
# ------------------------------------------------------------

echo "Starting Joulescope UI..."
echo "Python:"
"$PYTHON" --version
echo
echo "Environment:"
echo "  $VENV"
echo

exec "$PYTHON" -m joulescope_ui
