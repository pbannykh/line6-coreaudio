#!/bin/bash
#
# Uninstaller for a prebuilt release. Run WITHOUT sudo (sudo is used only to
# remove the plugin).
#
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
APP_DIR="$HOME/Library/Application Support/line6-coreaudio"

echo "==> Removing the launchd agent and stopping the daemon..."
"$HERE/launchagent.sh" uninstall

echo "==> Removing the plugin (sudo) and restarting coreaudiod..."
sudo rm -rf "/Library/Audio/Plug-Ins/HAL/Line6.driver"
sudo killall coreaudiod || true

echo "==> Removing the daemon..."
rm -rf "$APP_DIR"

echo "Done."
