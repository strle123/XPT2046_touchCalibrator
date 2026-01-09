#!/bin/bash
# Usage:
#   cd <repo>/installation
#   bash ./install.sh
# install.sh - Automated installation script for XPT2046_touchCalibrator
# Usage: Run this script from the root of the repository after cloning.
#
# This script will:
#   1. Update package lists
#   2. Install required build tools (cmake, build-essential)
#   3. Enable SPI if not already enabled
#   4. (Optionally) Build the driver
#
# You can extend this script with additional steps as needed.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

echo "Do you want to install XPT2046_touchCalibrator? Type [yes/no]"

while true; do
    read answer
    if [[ "$answer" == "yes" ]]; then
        break
    elif [[ "$answer" == "no" ]]; then
        echo "Installation skipped."
        echo "You can install later any time by running:"
        echo "  cd \"$SCRIPT_DIR\" && bash ./install.sh"
        echo "  (or: bash ./xpt-install.sh if present)"
        exit 0
    else
        echo "Invalid input. Please type yes or no."
        echo "Do you want to install XPT2046_touchCalibrator? Type [yes/no]"
    fi
done

set -e

XPT_ETC_DIR="/etc/xpt2046"
GUI_SERVICE_FILE="$XPT_ETC_DIR/gui.service"
INSTALL_MARKER_FILE="$XPT_ETC_DIR/installed.marker"

choose_gui_service() {
    echo ""
    echo "Optional: choose your GUI systemd service (for auto stop/start during calibration)."
    echo "If you don't have a GUI service yet, you can skip this."
    echo ""
    echo "Enable this feature? Type [yes/no]"

    while true; do
        read -r enable
        if [[ "$enable" == "yes" ]]; then
            break
        elif [[ "$enable" == "no" ]]; then
            return 0
        else
            echo "Invalid input. Please type yes or no."
        fi
    done

    if ! command -v systemctl >/dev/null 2>&1; then
        echo "systemctl not found; cannot list services. Skipping."
        return 0
    fi

    local services_raw
    services_raw=$(systemctl list-units --type=service --state=running --no-legend --no-pager 2>/dev/null || true)

    if [[ -z "$services_raw" ]]; then
        echo "No running services found (or cannot query systemd). Skipping."
        return 0
    fi

    # Filter out common system services to reduce noise.
    # (User can still choose 'manual entry' below.)
    local services
    # NOTE: grep returns exit code 1 when it filters out everything.
    # With `set -e`, that would abort the whole installer, so we `|| true`.
    services=$(echo "$services_raw" | awk '{print $1}' | \
        grep -vE '^(systemd-|dbus|ssh|sshd|cron|rsyslog|wpa_supplicant|NetworkManager|networking|avahi-daemon|polkit|getty@|serial-getty@|user@|bluetooth|cups|triggerhappy|apt-daily|apt-daily-upgrade|ModemManager|containerd|docker|snapd|udisks2|accounts-daemon|irqbalance|rng-tools|rsync|ntp|systemd-timesyncd|xpt-uinputd\.service)$' || true)

    echo ""
    echo "Running services (filtered):"
    local i=1
    local svc
    local svc_array=()
    while IFS= read -r svc; do
        [[ -z "$svc" ]] && continue
        svc_array+=("$svc")
        printf "  %2d) %s\n" "$i" "$svc"
        i=$((i+1))
    done <<< "$services"

    if (( ${#svc_array[@]} == 0 )); then
        echo "  (No services found after filtering.)"
    fi

    echo ""
    echo "Select a number, or type 'm' for manual entry, or press Enter to skip:"
    read -r selection
    if [[ -z "$selection" ]]; then
        return 0
    fi

    local chosen=""
    if [[ "$selection" == "m" ]]; then
        echo "Enter your GUI service name (example: myapp.service):"
        read -r chosen
    elif [[ "$selection" =~ ^[0-9]+$ ]]; then
        local idx=$((selection-1))
        if (( idx >= 0 && idx < ${#svc_array[@]} )); then
            chosen="${svc_array[$idx]}"
        else
            echo "Invalid selection. Skipping."
            return 0
        fi
    else
        echo "Invalid input. Skipping."
        return 0
    fi

    # Basic sanity: enforce .service suffix
    if [[ -n "$chosen" && ! "$chosen" =~ \.service$ ]]; then
        chosen="$chosen.service"
    fi

    if [[ -z "$chosen" ]]; then
        echo "No service selected. Skipping."
        return 0
    fi

    echo "Saving GUI service: $chosen"
    sudo mkdir -p "$XPT_ETC_DIR"
    echo "$chosen" | sudo tee "$GUI_SERVICE_FILE" >/dev/null
    echo "Saved to $GUI_SERVICE_FILE"
}

echo "[1/4] Updating package lists..."
sudo apt update

echo "[2/4] Installing build dependencies (cmake, build-essential, SDL2)..."
sudo apt install -y cmake build-essential libsdl2-dev libsdl2-2.0-0

echo "[3/4] Enabling SPI..."
sudo raspi-config nonint do_spi 0

sudo usermod -aG spi $USER

echo "[4/4] Building XPT2046 driver..."
cd "$REPO_DIR"
rm -rf build
mkdir build
cd build
cmake ..
make -j$(nproc)

choose_gui_service

# Mark the system as installed (used by calibrate.sh to avoid running on a fresh system
# before dependencies/services are set up).
sudo mkdir -p "$XPT_ETC_DIR"
echo "installed_at=$(date -Is 2>/dev/null || date)" | sudo tee "$INSTALL_MARKER_FILE" >/dev/null

echo "Installation complete!"
cd "$SCRIPT_DIR"
chmod +x calibrate.sh xpt-calibrate.sh configure_gui_service.sh install_uinput_service.sh uninstall.sh 2>/dev/null || true
echo "Tip: You can configure the GUI service later by running:"
echo "  cd \"$SCRIPT_DIR\" && bash ./configure_gui_service.sh"
echo "Tip (touch for any app): To expose SPI touch as a real Linux input device via uinput, run:"
echo "  cd \"$SCRIPT_DIR\" && bash ./install_uinput_service.sh"
echo "Tip: To run calibration and auto-stop/restore your GUI service, run:"
echo "  cd \"$SCRIPT_DIR\" && bash ./xpt-calibrate.sh"
if [[ -f ./xpt-calibrate.sh ]]; then
    bash ./xpt-calibrate.sh
else
    chmod +x calibrate.sh
    ./calibrate.sh
fi

echo ""
echo "Optional: install system-wide touch service (uinput) so all apps receive touch events."
echo "Install/start xpt-uinputd.service now? Type [yes/no]"
while true; do
    read -r ans
    case "${ans,,}" in
        yes)
            bash ./install_uinput_service.sh
            break
            ;;
        no)
            echo "Skipped installing uinput service. You can do it later with:"
            echo "  cd \"$SCRIPT_DIR\" && bash ./install_uinput_service.sh"
            break
            ;;
        *)
            echo "Invalid input. Please type yes or no."
            ;;
    esac
done
