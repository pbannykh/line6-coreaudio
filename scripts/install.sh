#!/bin/bash
#
# One-shot installer: daemon + HAL plugin + launchd agent.
#
#   scripts/install.sh                 # always-on, 64 ms latency
#   scripts/install.sh ondemand        # on-demand (start on connect)
#   scripts/install.sh ondemand 40     # on-demand, 40 ms latency
#   scripts/install.sh 40              # always-on, 40 ms latency
#
# Run WITHOUT sudo (it calls sudo only to install the plugin into
# /Library/Audio/Plug-Ins/HAL). The agent installs into the user domain.
#
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="$REPO/daemon/target/release/line6-daemon"
LABEL="org.line6.daemon"

MODE="always"; LAT="64"
for a in "$@"; do
    case "$a" in
        ondemand|always) MODE="$a" ;;
        ''|*[!0-9]*) ;;
        *) LAT="$a" ;;
    esac
done

echo "==> 1/4  Building the daemon (release)..."
( cd "$REPO/daemon" && cargo build --release )

echo "==> 2/4  Building the HAL plugin and haltest..."
( cd "$REPO/plugin" && make && make haltest )

echo "==> 3/4  haltest gate (testing the plugin without coreaudiod)..."
# Free the USB device: drop any agent and any daemon.
launchctl bootout "gui/$(id -u)/$LABEL" 2>/dev/null || true
pkill -f "line6-daemon daemon" 2>/dev/null || true
sleep 2
# Temporary --sine daemon for the IO simulation.
"$BIN" daemon --sine > /tmp/line6-haltest.log 2>&1 &
HPID=$!
sleep 2
RESULT="$("$REPO/plugin/haltest" 2>&1 | grep 'RESULT' || echo 'RESULT: FAIL (harness did not run)')"
kill "$HPID" 2>/dev/null || true
sleep 1
echo "    $RESULT"
case "$RESULT" in
    *PASS*) ;;
    *) echo "!! haltest failed - aborting (plugin NOT installed)."; exit 1 ;;
esac

echo "==> 4/4  Installing plugin (sudo) + launchd agent ($MODE, ${LAT} ms)..."
( cd "$REPO/plugin" && sudo make install )
"$REPO/scripts/launchagent.sh" "$MODE" "$LAT"

echo
echo "Done. The device appears in CoreAudio when a Line 6 device is connected."
echo "Daemon log: ~/Library/Logs/line6-daemon.log    Uninstall: scripts/uninstall.sh"
