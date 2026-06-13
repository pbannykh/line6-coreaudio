#!/bin/bash
#
# Installer for a prebuilt release (no Xcode / Rust needed). Expects the
# release artifacts next to this script: line6-daemon, Line6.driver, haltest,
# launchagent.sh.
#
#   ./install.sh                 # always-on, 64 ms latency
#   ./install.sh ondemand        # on-demand (start on connect)
#   ./install.sh ondemand 40     # on-demand, 40 ms latency
#
# Run WITHOUT sudo (it calls sudo only to install the plugin).
#
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
APP_DIR="$HOME/Library/Application Support/line6-coreaudio"
HALDIR="/Library/Audio/Plug-Ins/HAL"
DAEMON="$APP_DIR/line6-daemon"

MODE="always"; LAT="64"
for a in "$@"; do
    case "$a" in
        ondemand|always) MODE="$a" ;;
        ''|*[!0-9]*) ;;
        *) LAT="$a" ;;
    esac
done

echo "==> Removing the download quarantine..."
xattr -dr com.apple.quarantine "$HERE/line6-daemon" "$HERE/Line6.driver" "$HERE/haltest" 2>/dev/null || true
chmod +x "$HERE/line6-daemon" "$HERE/haltest"

echo "==> Testing the plugin (haltest property sweep)..."
if "$HERE/haltest" "$HERE/Line6.driver/Contents/MacOS/Line6HAL" --sweep-only | grep -q 'RESULT: PASS'; then
    echo "    PASS"
else
    echo "!! plugin self-test failed - aborting (the download may be corrupt)."; exit 1
fi

echo "==> Installing the daemon to $APP_DIR ..."
mkdir -p "$APP_DIR"
cp "$HERE/line6-daemon" "$DAEMON"
chmod +x "$DAEMON"
xattr -dr com.apple.quarantine "$DAEMON" 2>/dev/null || true

echo "==> Installing the plugin (sudo) and restarting coreaudiod..."
sudo rm -rf "$HALDIR/Line6.driver"
sudo cp -R "$HERE/Line6.driver" "$HALDIR/"
sudo xattr -dr com.apple.quarantine "$HALDIR/Line6.driver" 2>/dev/null || true
sudo killall coreaudiod || true

echo "==> Installing the launchd agent ($MODE, ${LAT} ms)..."
LINE6_DAEMON_BIN="$DAEMON" "$HERE/launchagent.sh" "$MODE" "$LAT"

echo
echo "Done. Connect a Line 6 device and pick it as the input in your DAW or in"
echo "System Settings > Sound. Uninstall: ./uninstall.sh"
