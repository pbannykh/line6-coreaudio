line6-coreaudio - prebuilt for macOS (universal: Apple Silicon + Intel)

Install (no Xcode or Rust needed):
  ./install.sh ondemand        start the helper when the device is connected (recommended)
  ./install.sh                 always-on
  ./install.sh ondemand 40     set 40 ms latency (default 64)

Uninstall:
  ./uninstall.sh

These binaries are ad-hoc signed, not notarized. The installer removes the macOS
download quarantine for you. Source and docs:
https://github.com/pbannykh/line6-coreaudio
