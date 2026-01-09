#!/bin/bash
# install_uinput_service.sh - Install/start the uinput touch daemon as a systemd service.
#
# Usage:
#   cd <repo>/installation
#   bash ./install_uinput_service.sh

set -euo pipefail

if ! command -v systemctl >/dev/null 2>&1; then
  echo "systemctl not found; cannot install service."
  exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
UNIT_SRC="$SCRIPT_DIR/systemd/xpt-uinputd.service"
BIN="$REPO_DIR/build/xpt2046_uinputd"
if [[ ! -x "$BIN" ]]; then
  echo "uinput daemon binary not found or not executable: $BIN"
  echo "Build it first: cd \"$REPO_DIR\" && mkdir -p build && cd build && cmake .. && make -j\$(nproc)"
  exit 1
fi

if [[ ! -f "$UNIT_SRC" ]]; then
  echo "Unit file not found: $UNIT_SRC"
  exit 1
fi

echo "Installing binary to /usr/local/bin/xpt2046_uinputd"
sudo install -m 0755 "$BIN" /usr/local/bin/xpt2046_uinputd

echo "Installing unit to /etc/systemd/system/xpt-uinputd.service"
sudo cp "$UNIT_SRC" /etc/systemd/system/xpt-uinputd.service
sudo systemctl daemon-reload
sudo systemctl enable --now xpt-uinputd.service

echo "xpt-uinputd.service is running."
