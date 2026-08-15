#!/usr/bin/env bash

set -Eeuo pipefail

readonly APP_NAME="Joulescope UI"
readonly VENV_DIR="${XDG_DATA_HOME:-${HOME}/.local/share}/joulescope-ui/venv"
readonly VENV_PYTHON="${VENV_DIR}/bin/python"
readonly UDEV_RULE="/etc/udev/rules.d/99-joulescope.rules"
readonly UDEV_RULE_URL="https://raw.githubusercontent.com/jetperch/joulescope_driver/main/99-joulescope.rules"

readonly -a REQUIRED_PACKAGES=(
    python3
    python3-venv
    python3-pip
    libudev-dev
    libxcb-cursor0
    wget
)

package_is_installed() {
    dpkg-query -W -f='${Status}' "$1" 2>/dev/null \
        | grep -q '^Status: install ok installed$'
}

ui_is_installed() {
    [[ -x "${VENV_PYTHON}" ]] && \
        "${VENV_PYTHON}" -c \
            'import importlib.util, sys; sys.exit(0 if importlib.util.find_spec("joulescope_ui") else 1)' \
            >/dev/null 2>&1
}

all_requirements_are_installed() {
    local package

    for package in "${REQUIRED_PACKAGES[@]}"; do
        package_is_installed "${package}" || return 1
    done

    ui_is_installed || return 1
    [[ -f "${UDEV_RULE}" ]] || return 1
}

launch_joulescope() {
    echo "Starting ${APP_NAME}..."
    exec "${VENV_PYTHON}" -m joulescope_ui "$@"
}

install_missing_requirements() {
    local -a missing_packages=()
    local package
    local temporary_rule=""

    for package in "${REQUIRED_PACKAGES[@]}"; do
        if ! package_is_installed "${package}"; then
            missing_packages+=("${package}")
        fi
    done

    if ((${#missing_packages[@]} > 0)); then
        echo "Installing missing Ubuntu packages: ${missing_packages[*]}"
        sudo apt update
        sudo apt install -y "${missing_packages[@]}"
    else
        echo "Required Ubuntu packages are already installed."
    fi

    if ! ui_is_installed; then
        echo "Creating the Joulescope Python environment..."
        mkdir -p "$(dirname "${VENV_DIR}")"
        python3 -m venv "${VENV_DIR}"

        "${VENV_PYTHON}" -m pip install --upgrade pip
        "${VENV_PYTHON}" -m pip install \
            --upgrade \
            --upgrade-strategy=eager \
            joulescope_ui
    else
        echo "${APP_NAME} is already installed."
    fi

    if [[ ! -f "${UDEV_RULE}" ]]; then
        echo "Installing the Joulescope USB permission rule..."
        temporary_rule="$(mktemp --tmpdir joulescope-rule.XXXXXX)"
        trap 'rm -f "${temporary_rule:-}"' RETURN

        wget --https-only -O "${temporary_rule}" "${UDEV_RULE_URL}"

        if [[ ! -s "${temporary_rule}" ]]; then
            echo "Error: downloaded USB permission rule is empty." >&2
            return 1
        fi

        sudo install -m 0644 "${temporary_rule}" "${UDEV_RULE}"
        sudo udevadm control --reload-rules
        sudo udevadm trigger

        echo "USB permission rule installed."
        echo "If the Joulescope is already connected, unplug and reconnect it once."
    else
        echo "Joulescope USB permission rule is already installed."
    fi
}

main() {
    if all_requirements_are_installed; then
        launch_joulescope "$@"
    fi

    echo "First-time setup or repair is required."
    install_missing_requirements

    if ! all_requirements_are_installed; then
        echo "Error: ${APP_NAME} setup did not complete successfully." >&2
        exit 1
    fi

    echo "Setup completed successfully."
    launch_joulescope "$@"
}

main "$@"
