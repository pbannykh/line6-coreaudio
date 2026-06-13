# Line6 HAL plugin

A CoreAudio `AudioServerPlugin` that presents a Line 6 device as a macOS input.
It reads audio from the shared-memory ring filled by the daemon (`../daemon`) and
is device-agnostic: the channel count and model name come from the shared-memory
header, so one plugin serves any device the daemon supports.

## Build / install

```sh
make            # build Line6.driver (bundle) + ad-hoc signature
make haltest    # build the in-process test harness
sudo make install    # copy to /Library/Audio/Plug-Ins/HAL and restart coreaudiod
sudo make uninstall  # remove and restart coreaudiod
```

Usually installed by `../scripts/install.sh`, which runs the haltest gate first.

## ⚠️ Always run haltest before installing

`haltest` `dlopen`s the plugin **in its own process, without coreaudiod**, and:

1. **Property sweep** — for every object × selector × scope × element it checks
   that `HasProperty` / `GetPropertyDataSize` / `GetPropertyData` agree and that
   only the *Main* element is answered. A mismatch here can make coreaudiod spin
   at 100% CPU, so this is a hard gate.
2. **IO simulation** — drives `StartIO` → `GetZeroTimeStamp` / `DoIO(ReadInput)`
   and verifies the captured signal is a continuous tone.

```sh
# needs a running daemon producing a known signal:
( cd ../daemon && cargo run --release -- daemon --sine ) &
make haltest && ./haltest        # expect "RESULT: PASS (0 problems)"
```

`scripts/install.sh` does this automatically (it starts a temporary `--sine`
daemon, checks `PASS`, then installs).

## Notes

- Timeline: a stable host-clock 48 kHz timeline (`GetZeroTimeStamp`) plus a read
  cursor advanced once per IO cycle. Buffer depth comes from `latency_frames` in
  shared memory (set by the daemon's `--latency-ms`).
- Presence: a background thread watches `write_frames`; when it stops growing the
  device is removed from the device list, so unplugging behaves like any USB
  interface and there is never an orphaned default device.
- Only the *Main* element is answered and `GetPropertyDataSize` is kept in sync
  with `GetPropertyData`; both rules are load-bearing for coreaudiod stability.
