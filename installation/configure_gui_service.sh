#!/bin/bash
# configure_gui_service.sh - Configure which GUI systemd service should be stopped/started during calibration.
# Usage:
#   cd <repo>/installation
#   bash ./configure_gui_service.sh
# Options:
#   --clear   Remove saved GUI service selection

set -euo pipefail

XPT_ETC_DIR="/etc/xpt2046"
GUI_SERVICE_FILE="$XPT_ETC_DIR/gui.service"

if [[ "${1:-}" == "--clear" ]]; then
  echo "Clearing $GUI_SERVICE_FILE"
  sudo rm -f "$GUI_SERVICE_FILE"
  echo "Done."
  exit 0
fi

if ! command -v systemctl >/dev/null 2>&1; then
  echo "systemctl not found; cannot list services."
  echo "You can still set it manually by writing one line to: $GUI_SERVICE_FILE"
  exit 1
fi

services_raw=$(systemctl list-units --type=service --state=running --no-legend --no-pager 2>/dev/null || true)

if [[ -z "$services_raw" ]]; then
  echo "No running services found (or cannot query systemd)."
  echo "Manual entry still works."
fi

# Filter out common system services to reduce noise.
filtered=$(echo "$services_raw" | awk '{print $1}' | \
  grep -vE '^(systemd-|dbus|ssh|sshd|cron|rsyslog|wpa_supplicant|NetworkManager|networking|avahi-daemon|polkit|getty@|serial-getty@|user@|bluetooth|cups|triggerhappy|apt-daily|apt-daily-upgrade|ModemManager|containerd|docker|snapd|udisks2|accounts-daemon|irqbalance|rng-tools|rsync|ntp|systemd-timesyncd|xpt-uinputd\.service)$' || true)

svc_array=()

echo ""
echo "GUI service selection"
echo "- Pick the service that runs your fullscreen GUI/kiosk app (Qt/SDL/etc.)"
echo "- This will be stopped during calibration and started again on Save & Exit"
echo ""

echo "Running services (filtered):"
i=1
while IFS= read -r svc; do
  [[ -z "$svc" ]] && continue
  svc_array+=("$svc")
  printf "  %2d) %s\n" "$i" "$svc"
  i=$((i+1))
done <<< "$filtered"

if (( ${#svc_array[@]} == 0 )); then
  echo "  (None found after filtering.)"
  echo ""
  echo "Tip: If your GUI service isn't running right now, start it first, then re-run this script."
fi

echo ""
echo "Select a number, or type 'm' for manual entry, or press Enter to cancel:"
read -r selection

if [[ -z "$selection" ]]; then
  echo "Cancelled."
  exit 0
fi

chosen=""
if [[ "$selection" == "m" ]]; then
  echo "Enter your GUI service name (example: myapp.service):"
  read -r chosen
elif [[ "$selection" =~ ^[0-9]+$ ]]; then
  idx=$((selection-1))
  if (( idx >= 0 && idx < ${#svc_array[@]} )); then
    chosen="${svc_array[$idx]}"
  else
    echo "Invalid selection."
    exit 1
  fi
else
  echo "Invalid input."
  exit 1
fi

# Basic sanity: enforce .service suffix
if [[ -n "$chosen" && ! "$chosen" =~ \.service$ ]]; then
  chosen="$chosen.service"
fi

if [[ -z "$chosen" ]]; then
  echo "No service selected."
  exit 1
fi

echo "Saving GUI service: $chosen"
sudo mkdir -p "$XPT_ETC_DIR"
echo "$chosen" | sudo tee "$GUI_SERVICE_FILE" >/dev/null

echo "Saved to $GUI_SERVICE_FILE"