# Architecture

```
 Line 6 device  ──USB iso──▶  line6-daemon  ──POSIX shm──▶  Line6 HAL plugin  ──▶  CoreAudio / apps
                             (user process)   (lock-free ring)  (inside coreaudiod)
```

Two processes share a POSIX shared-memory segment. The daemon is the only writer,
the plugin the only reader; they synchronise with a single atomic counter.

## Components

- **`daemon/`** (Rust) — opens the USB device with libusb, runs isochronous
  transfers, converts samples, and writes interleaved Float32 frames into the
  ring. Also: device detection, auto-reconnect (watchdog), and start/stop with
  the device.
- **`plugin/`** (C) — a CoreAudio `AudioServerPlugin` bundle loaded by
  `coreaudiod`. Presents one input device, reads frames from the ring, and adds /
  removes the device as the hardware appears / disappears.
- **`shared/line6_shm.h`** — the byte-for-byte layout both sides agree on, mirrored
  in `daemon/src/shm.rs` (a compile-time assert checks the struct size).

## Shared memory (`/line6_audio`)

A header followed by a ring of `RING_FRAMES` (65536) frames of interleaved
Float32. Key header fields:

- `channels`, `sample_rate`, `name` — describe the connected device (the daemon
  fills these from its device table). The plugin reads them, so it is not tied to
  one model.
- `latency_frames` — buffer depth requested by the daemon (`--latency-ms`).
- `write_frames` — total frames written, free-running. The write position is
  `write_frames & RING_MASK`; the read position trails it by the latency depth.

The segment is **persistent** (never unlinked), so the daemon can restart without
the plugin needing to reattach.

## Daemon

### USB capture (`usb.rs`)

libusb isochronous transfers (`nusb` has no isochronous support on macOS). The
flow mirrors the Linux `sound/usb/line6` driver:

- Claim the audio interface at its active alternate setting.
- Run IN transfers (capture) **and** OUT transfers carrying silence. The OUT
  stream is mandatory: the device clocks capture off it (`IN_NEEDS_OUT`).
- Each IN packet is framed **independently**: take whole frames, drop any partial
  tail, never carry a partial frame across packets. Carrying a tail would shift
  the frame boundary and rotate the channels permanently. (This matches Linux
  `line6_capture_copy`.)
- The capture thread runs at `USER_INTERACTIVE` QoS so the scheduler does not
  preempt isochronous processing under load.

### Main loop & watchdog (`main.rs`)

A single loop holds an optional running engine:

- **No engine:** look for a supported device (`devices::find_connected`). If found,
  (re)create the shm for it and start capture. If the device is present but busy
  (e.g. racing a previous instance) keep retrying. With `--exit-when-absent`, exit
  cleanly when no device is present (so launchd can restart on connect).
- **Engine running:** service USB events. If `write_frames` stops growing for
  >0.5 s the capture has wedged or the device was unplugged — tear the engine down
  so the next iteration rebuilds it (or exits).

### Device table (`devices.rs`)

`DEVICES` is a static table; each row gives a model's USB id, audio interface,
alternate setting, endpoints, and channel counts. Adding a device is adding a row.
See [ADDING_DEVICES.md](ADDING_DEVICES.md).

## Plugin

### Timeline

`GetZeroTimeStamp` returns a stable, monotone 48 kHz timeline anchored to the host
clock: every period boundary lies exactly on a line through the anchor, so the HAL
sees a perfectly consistent timeline and a constant seed. It is not sample-locked
to the device clock — a small drift remains (see Limitations in the README) — but
it never flaps or storms.

### Read cursor

`DoIO(ReadInput)` advances a read cursor once per IO cycle (keyed on
`mIOCycleCounter`), so several clients in one cycle read the same contiguous
position. The cursor trails the write head by `max(latency_frames, 2*buffer +
safety)`; if it reaches or passes the write head it resyncs into that window.

### Presence monitor

A background thread polls `write_frames` every 0.5 s. If it stops growing for ~2 s
(or the shm is gone) the device is removed from the plugin's device list via
`PropertiesChanged(kAudioPlugInPropertyDeviceList)`; when it grows again the device
reappears. This makes unplugging behave like any USB interface and avoids an
orphaned default device.

### coreaudiod stability rules

Two rules are load-bearing — violating either can make `coreaudiod` spin at 100%
CPU, so `haltest` checks both:

1. Answer **only** the *Main* element. Answering arbitrary elements sends the HAL
   proxy into an infinite object enumeration.
2. Keep `GetPropertyDataSize` and `GetPropertyData` **consistent** (same size, same
   data) for every object/scope.

## Why not DriverKit?

A DriverKit driver (DEXT) would be the "proper" native path, but the USB/audio
DriverKit entitlements require a **paid** Apple Developer account and Apple's
approval. The HAL-plugin + daemon approach needs neither and fully covers input
capture.
