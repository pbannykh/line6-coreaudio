#!/bin/bash
#
# Install/remove the launchd agent that runs the capture daemon. Two modes:
#
#   scripts/launchagent.sh [latency_ms]            # ALWAYS-ON: KeepAlive +
#                                          #   RunAtLoad. The daemon always runs
#                                          #   (idles cheaply with no device,
#                                          #   picks up a device when connected).
#   scripts/launchagent.sh ondemand [latency_ms]   # ON-DEMAND: launchd starts the
#                                          #   daemon when a device is connected
#                                          #   (LaunchEvents / IOKit matching) and
#                                          #   the daemon exits when it is removed.
#   scripts/launchagent.sh uninstall       # stop and remove
#
# latency_ms (default 64) sets the plugin buffer depth: lower = lower latency but
# needs a faster machine. Run WITHOUT sudo (a user agent).
#
set -euo pipefail

LABEL="org.line6.daemon"
PLIST="$HOME/Library/LaunchAgents/$LABEL.plist"
LOG="$HOME/Library/Logs/line6-daemon.log"
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="$REPO/daemon/target/release/line6-daemon"
UID_NUM="$(id -u)"
DOMAIN="gui/$UID_NUM"

# Arguments in any order: a mode keyword and/or a number (latency_ms).
MODE="always"; LAT_MS="64"
for arg in "$@"; do
    case "$arg" in
        ondemand|always|uninstall) MODE="$arg" ;;
        ''|*[!0-9]*) ;;
        *) LAT_MS="$arg" ;;
    esac
done

if [ "$MODE" = "uninstall" ]; then
    launchctl bootout "$DOMAIN/$LABEL" 2>/dev/null || true
    rm -f "$PLIST"
    echo "removed $LABEL"
    exit 0
fi

if [ ! -x "$BIN" ]; then
    echo "release binary not found, building..."
    ( cd "$REPO/daemon" && cargo build --release )
fi

mkdir -p "$HOME/Library/LaunchAgents" "$HOME/Library/Logs"

# Line 6 USB ids for IOKit launch matching (on-demand). Vendor 0x0E41 = 3649,
# POD X3 Live 0x414B = 16715, POD X3 0x414A = 16714. Add a block per product id
# when you add a device to daemon/src/devices.rs.
if [ "$MODE" = "ondemand" ]; then
    cat > "$PLIST" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Label</key>
    <string>$LABEL</string>
    <key>ProgramArguments</key>
    <array>
        <string>$BIN</string>
        <string>daemon</string>
        <string>--exit-when-absent</string>
        <string>--latency-ms</string>
        <string>$LAT_MS</string>
    </array>
    <key>RunAtLoad</key>
    <false/>
    <key>KeepAlive</key>
    <dict>
        <key>SuccessfulExit</key>
        <false/>
    </dict>
    <key>ProcessType</key>
    <string>Interactive</string>
    <key>LaunchEvents</key>
    <dict>
        <key>com.apple.iokit.matching</key>
        <dict>
            <key>podx3live</key>
            <dict>
                <key>IOProviderClass</key>
                <string>IOUSBHostDevice</string>
                <key>idVendor</key>
                <integer>3649</integer>
                <key>idProduct</key>
                <integer>16715</integer>
            </dict>
            <key>podx3</key>
            <dict>
                <key>IOProviderClass</key>
                <string>IOUSBHostDevice</string>
                <key>idVendor</key>
                <integer>3649</integer>
                <key>idProduct</key>
                <integer>16714</integer>
            </dict>
        </dict>
    </dict>
    <key>StandardOutPath</key>
    <string>$LOG</string>
    <key>StandardErrorPath</key>
    <string>$LOG</string>
</dict>
</plist>
EOF
    MODE_DESC="on-demand (starts on connect, exits on removal)"
else
    cat > "$PLIST" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Label</key>
    <string>$LABEL</string>
    <key>ProgramArguments</key>
    <array>
        <string>$BIN</string>
        <string>daemon</string>
        <string>--latency-ms</string>
        <string>$LAT_MS</string>
    </array>
    <key>RunAtLoad</key>
    <true/>
    <key>KeepAlive</key>
    <true/>
    <key>ProcessType</key>
    <string>Interactive</string>
    <key>StandardOutPath</key>
    <string>$LOG</string>
    <key>StandardErrorPath</key>
    <string>$LOG</string>
</dict>
</plist>
EOF
    MODE_DESC="always-on"
fi

# Drop a previous agent and any manual daemon so USB is free, then load.
launchctl bootout "$DOMAIN/$LABEL" 2>/dev/null || true
pkill -f "$BIN daemon" 2>/dev/null || true
pkill -f "line6-daemon daemon" 2>/dev/null || true
sleep 3

if ! launchctl bootstrap "$DOMAIN" "$PLIST" 2>/dev/null; then
    echo "  (first bootstrap failed, retrying in 2s...)"
    launchctl bootout "$DOMAIN/$LABEL" 2>/dev/null || true
    sleep 2
    launchctl bootstrap "$DOMAIN" "$PLIST"
fi

echo "installed $LABEL"
echo "  mode:    $MODE_DESC"
echo "  latency: $LAT_MS ms"
echo "  log:     $LOG"
