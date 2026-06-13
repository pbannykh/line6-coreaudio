# line6-coreaudio

[![CI](https://github.com/pbannykh/line6-coreaudio/actions/workflows/ci.yml/badge.svg)](https://github.com/pbannykh/line6-coreaudio/actions/workflows/ci.yml)

A native **audio input driver for Line 6 USB devices on macOS** (Apple Silicon),
for hardware that has no working macOS driver — such as the **Line 6 POD X3 /
POD X3 Live**.

It runs entirely in user space: **no kernel extension, no DriverKit, and no paid
Apple Developer account**. Once installed, the device shows up like any USB audio
interface, and you can pick it as the input in your DAW (Logic, Reaper, GarageBand,
…) or in **System Settings → Sound**.

> **Status:** capture (recording) works and is stable. Playback **to** the device
> (using it as an output) is not implemented yet — see [Limitations](#limitations).

## Supported devices

| Device | USB id | Capture channels |
|--------|--------|------------------|
| POD X3 Live | `0E41:414B` | 8 |
| POD X3 | `0E41:414A` | 8 |

Other Line 6 USB devices that the Linux kernel driver supports share the same
transport and can be added — see [docs/ADDING_DEVICES.md](docs/ADDING_DEVICES.md).

The POD X3 sends 8 channels to the computer:

| Channels | Signal |
|----------|--------|
| 1–2 | Main output (stereo) |
| 3–4 | Tone 1 (stereo) |
| 5–6 | Tone 2 (stereo) |
| 7 | Tone 1 dry input (mono) |
| 8 | Tone 2 dry input (mono) |

---

## Install (step by step)

You only need to do this once. Open the **Terminal** app and run the commands
below (copy and paste each line, press Return).

**1. Install Apple's Command Line Tools** (skip if you already have them):

```sh
xcode-select --install
```

**2. Install Rust** (the daemon is written in Rust):

```sh
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh
```

Then open a new Terminal window so `cargo` is on your PATH.

**3. Get this project and install it:**

```sh
cd ~/Downloads
git clone https://github.com/pbannykh/line6-coreaudio.git
cd line6-coreaudio
./scripts/install.sh ondemand
```

The installer builds everything, runs a self-test, asks for your password once
(to install the audio plugin), and sets it up to start automatically when you
plug the device in. That's it.

**4. Use it.** Plug in your POD X3 and select it as the input device in your DAW
or in **System Settings → Sound → Input**.

### Choosing how the daemon runs

`./scripts/install.sh` takes options:

- `ondemand` — the helper starts when you connect the device and stops when you
  unplug it (recommended).
- *(no argument)* — "always-on": the helper is always running and picks the
  device up when connected.
- a number, e.g. `40` — set the buffer latency in milliseconds (default 64).
  Lower is snappier but needs a faster Mac. Example:
  `./scripts/install.sh ondemand 40`.

### Uninstall

```sh
./scripts/uninstall.sh
```

---

## How it works (short version)

Two pieces talk through a shared-memory ring:

```
 POD X3  ──USB──▶  line6-daemon  ──shared memory──▶  Line6 HAL plugin  ──▶  CoreAudio
                  (captures audio)                  (in coreaudiod)
```

- **`daemon/`** — a small Rust program that reads audio off the USB device with
  libusb and writes it into shared memory. It also reconnects automatically and
  starts/stops with the device.
- **`plugin/`** — a CoreAudio *AudioServerPlugin* (a `.driver` bundle loaded by
  macOS's `coreaudiod`) that reads the shared memory and presents a normal input
  device. It adds/removes the device as you plug/unplug the hardware.
- **`shared/line6_shm.h`** — the memory layout both sides agree on.

More detail in [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) and the USB protocol
in [docs/PROTOCOL.md](docs/PROTOCOL.md).

## Limitations

- **Input only.** Using the device as an *output* (playback / re-amping) is not
  implemented.
- A tiny clock difference between the Mac and the device can cause one faint click
  every few minutes in very long sessions.

## Building manually (for developers)

```sh
cd daemon  && cargo build --release           # the capture daemon
cd ../plugin && make && make haltest           # the HAL plugin + its test harness
```

`haltest` loads the plugin in-process (no `coreaudiod`) and checks it. **Always**
run it and confirm `RESULT: PASS` before installing the plugin — a buggy plugin
can make `coreaudiod` spin at 100% CPU. `scripts/install.sh` does this gate for
you. See [plugin/README.md](plugin/README.md).

## Credits

The USB protocol and device parameters were reconstructed from the Linux kernel
driver `sound/usb/line6/` (`snd-usb-podhd`). Thanks to its authors. This project
is an independent macOS reimplementation; it is not affiliated with or endorsed
by Line 6 / Yamaha.
