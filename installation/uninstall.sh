#!/bin/bash
# uninstall.sh - Uninstall the *calibrator tooling* and (optionally) the system touch service.
#
# Goals:
#   - After uninstall, calibration should not be possible (marker removed).
#   - User can keep system-wide touch working (xpt-uinputd.service + config) to avoid breaking apps.
#
# This script also cleans up any previously-installed test GUI service units (if present):
#   - xpt-test-gui@.service (and any running xpt-test-gui@*.service instances)
#
# It optionally removes the uinput touch service:
#   - xpt-uinputd.service (+ /usr/local/bin/xpt2046_uinputd)
#
# Usage:
#   cd <repo>/installation
#   bash ./uninstall.sh

set -euo pipefail

XPT_ETC_DIR="/etc/xpt2046"
CALIB_FILE="$XPT_ETC_DIR/touch_config.txt"
MARKER_FILE="$XPT_ETC_DIR/installed.marker"
GUI_SERVICE_FILE="$XPT_ETC_DIR/gui.service"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
REPO_BASENAME="$(basename "$REPO_DIR")"

echo "This will uninstall XPT2046_touchCalibrator from this system."
echo ""

ask_yes_no() {
  local prompt="$1"
  local ans=""
  while true; do
    echo "$prompt Type [yes/no]" >&2
    read -r ans
    ans="$(echo "$ans" | tr '[:upper:]' '[:lower:]' | xargs)"
    if [[ "$ans" == "y" || "$ans" == "yes" ]]; then
      echo "yes"
      return 0
    fi
    if [[ "$ans" == "n" || "$ans" == "no" ]]; then
      echo "no"
      return 0
    fi
    echo "Please answer yes or no." >&2
  done
}

keep_touch="$(ask_yes_no "Keep system touch working (keep xpt-uinputd.service + $CALIB_FILE)?" "yes")"

keep_cfg="yes"
if [[ "$keep_touch" == "no" ]]; then
  keep_cfg="$(ask_yes_no "Keep calibration config file ($CALIB_FILE)?" "yes")"
fi

delete_repo="no"
if [[ "$REPO_BASENAME" == "XPT2046_touchCalibrator" ]]; then
  delete_repo="$(ask_yes_no "Delete project directory too ($REPO_DIR)?" "no")"
fi

have_systemctl=0
if command -v systemctl >/dev/null 2>&1; then
  have_systemctl=1
fi

if [[ "$have_systemctl" -eq 1 ]]; then
  echo "Stopping/disabling services (if present)..."

  if [[ "$keep_touch" == "no" ]]; then
    sudo systemctl disable --now xpt-uinputd.service >/dev/null 2>&1 || true
  fi
  sudo systemctl disable --now xpt-test-gui.service >/dev/null 2>&1 || true

  # Disable any templated instances currently known to systemd.
  instances=$(systemctl list-units --all 'xpt-test-gui@*.service' --no-legend --no-pager 2>/dev/null | awk '{print $1}' || true)
  if [[ -n "$instances" ]]; then
    while IFS= read -r svc; do
      [[ -z "$svc" ]] && continue
      sudo systemctl disable --now "$svc" >/dev/null 2>&1 || true
    done <<< "$instances"
  fi

  echo "Removing unit files (if present)..."
  if [[ "$keep_touch" == "no" ]]; then
    sudo rm -f /etc/systemd/system/xpt-uinputd.service
  fi
  sudo rm -f /etc/systemd/system/xpt-test-gui@.service
  sudo rm -f /etc/systemd/system/xpt-test-gui.service

  sudo systemctl daemon-reload
else
  echo "systemctl not found; skipping service stop/removal."
fi

echo "Removing installed binaries (if present)..."
if [[ "$keep_touch" == "no" ]]; then
  sudo rm -f /usr/local/bin/xpt2046_uinputd
fi

# Always remove marker + GUI service selection so calibration can't run after uninstall.
sudo rm -f "$MARKER_FILE" "$GUI_SERVICE_FILE" >/dev/null 2>&1 || true

if [[ "$keep_touch" == "yes" ]]; then
  echo "Keeping system touch service (xpt-uinputd.service) and calibration config."
  echo "Note: calibration tooling is considered uninstalled (marker removed)."
else
  if [[ "$keep_cfg" == "no" ]]; then
    echo "Removing $XPT_ETC_DIR ..."
    sudo rm -rf "$XPT_ETC_DIR"
  else
    echo "Keeping $CALIB_FILE (but removing marker/gui.service)."
    sudo mkdir -p "$XPT_ETC_DIR" >/dev/null 2>&1 || true
  fi
fi

if [[ "$delete_repo" == "yes" ]]; then
  echo "Scheduling deletion of $REPO_DIR ..."
  # Delete after this script exits to avoid removing the running script.
  nohup bash -c "sleep 1; rm -rf '$REPO_DIR'" >/dev/null 2>&1 &
fi

echo "Uninstall complete."
