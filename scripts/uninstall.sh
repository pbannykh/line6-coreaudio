#!/bin/bash
#
# Remove everything: launchd agent + HAL plugin.
# Run WITHOUT sudo (sudo is used only to remove the plugin).
#
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

echo "==> Removing the launchd agent and stopping the daemon..."
"$REPO/scripts/launchagent.sh" uninstall

echo "==> Removing the HAL plugin (sudo) and restarting coreaudiod..."
( cd "$REPO/plugin" && sudo make uninstall )

echo
echo "Done. The shared memory segment /line6_audio is harmless and left in place."
