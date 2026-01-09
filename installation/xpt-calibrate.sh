#!/bin/bash
# xpt-calibrate.sh - Stop configured GUI service, run calibration wizard, then restore GUI service.
#
# Expected config file:
#   /etc/xpt2046/gui.service   (single line: <service-name>.service)
#
# Usage:
#   cd <repo>/installation
#   bash ./xpt-calibrate.sh

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
GUI_SERVICE_FILE="/etc/xpt2046/gui.service"
INSTALL_MARKER_FILE="/etc/xpt2046/installed.marker"

# Calibration is only supported after running the installer.
if [[ ! -f "$INSTALL_MARKER_FILE" ]]; then
  echo "XPT2046_touchCalibrator is not installed on this system yet."
  echo "Run: cd \"$SCRIPT_DIR\" && bash ./install.sh"
  exit 1
fi

chosen_service=""
if [[ -f "$GUI_SERVICE_FILE" ]]; then
  chosen_service="$(tr -d '\r' < "$GUI_SERVICE_FILE" | head -n1 | xargs || true)"
fi

if [[ "$chosen_service" == "xpt-uinputd.service" ]]; then
  echo "Warning: $GUI_SERVICE_FILE points to xpt-uinputd.service (not a GUI)."
  echo "Run: bash ./configure_gui_service.sh and select your real GUI service."
  chosen_service=""
fi

have_systemctl=0
if command -v systemctl >/dev/null 2>&1; then
  have_systemctl=1
fi

uinput_service="xpt-uinputd.service"
have_uinput=0
if [[ "$have_systemctl" -eq 1 ]]; then
  if systemctl list-unit-files --no-pager 2>/dev/null | awk '{print $1}' | grep -qx "$uinput_service"; then
    have_uinput=1
  fi
fi

restored=0
restore_gui() {
  if [[ "$restored" -eq 1 ]]; then
    return 0
  fi
  if [[ "$have_uinput" -eq 1 ]]; then
    sudo systemctl start "$uinput_service" >/dev/null 2>&1 || true
  fi
  if [[ -n "$chosen_service" && "$have_systemctl" -eq 1 ]]; then
    sudo systemctl start "$chosen_service" >/dev/null 2>&1 || true
  fi
  restored=1
}
trap restore_gui EXIT INT TERM

if [[ -n "$chosen_service" && "$have_systemctl" -eq 1 ]]; then
  echo "Stopping GUI service: $chosen_service"
  sudo systemctl stop "$chosen_service"
else
  echo "No GUI service configured (or systemctl missing). Running calibration only."
  echo "Tip: configure with: bash ./configure_gui_service.sh"
fi

if [[ "$have_uinput" -eq 1 ]]; then
  echo "Stopping uinput service: $uinput_service"
  sudo systemctl stop "$uinput_service" || true
fi

bash "$SCRIPT_DIR/calibrate.sh"
status=$?

restore_gui
exit "$status"
