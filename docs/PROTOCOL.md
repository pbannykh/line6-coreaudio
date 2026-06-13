# Line 6 USB protocol (POD X3 family)

Reconstructed from the Linux kernel driver `sound/usb/line6/` (module
`snd-usb-podhd`) and confirmed against POD X3 hardware. Values below are for the
POD X3 / X3 Live; other devices differ in endpoints and channel counts (see the
`*_properties_table` entries in the kernel driver, and [ADDING_DEVICES.md](ADDING_DEVICES.md)).

## Device

- Vendor id `0x0E41` (Line 6). Product id `0x414B` (POD X3 Live), `0x414A` (POD X3).
- Vendor-specific device class (`0xFF`) — not USB Audio Class, so macOS has no
  built-in driver.
- High-speed device: 8000 microframes per second.

## Interfaces and endpoints

| Interface | Purpose | Endpoints |
|-----------|---------|-----------|
| 0 | Audio (isochronous) | IN `0x86`, OUT `0x02` |
| 1 | Control (bulk) | IN `0x81`, OUT `0x01` |

The audio endpoints are active in **alternate setting 1** of interface 0 (alt 0 is
the disabled/idle setting). The driver claims interface 0 and selects alt 1.

## Audio format

- Sample format: **S24_3LE** (24-bit signed, little-endian, packed in 3 bytes).
- Sample rate: **48 kHz**, fixed.
- Capture: 8 channels. Playback: 2 channels.
- At high speed there are ~6 frames per isochronous packet (48000 / 8000), with
  occasional 7-frame packets from clock jitter. A packet is always a whole number
  of frames and starts on a frame (channel-0) boundary.

### Capture channel layout (POD X3)

| Channels | Signal |
|----------|--------|
| 1–2 | Main output (stereo, as configured for the Digital/XLR outs) |
| 3–4 | Tone 1 (stereo) |
| 5–6 | Tone 2 (stereo) |
| 7 | Sum of the inputs assigned to Tone 1 (dry, mono) |
| 8 | Sum of the inputs assigned to Tone 2 (dry, mono) |

(Channels 7 and 8 are two separate mono dry signals, not a stereo pair.)

### IN_NEEDS_OUT

Capture does not run unless an OUT stream runs at the same time: the device clocks
its capture off the playback stream. The daemon therefore always submits OUT
transfers carrying silence while capturing, even though playback is not used.

### Framing caveat

Each isochronous IN packet starts on a frame boundary, so frame each packet
independently: `frames = actual_length / (channels * 3)`, write that many whole
frames, and discard any partial tail. Concatenating packets and re-framing across
boundaries can leave a fixed sub-frame offset that rotates every channel by one
permanently. (The Linux driver does the per-packet thing in `line6_capture_copy`.)

## Control protocol (optional)

Not required for audio capture, but documented for completeness. Control messages
go to endpoint 0 with a single vendor request `bRequest = 0x67`:

- **Read** memory: a write request `wValue = (len << 8) | 0x21, wIndex = address`,
  then poll a 1-byte status (`wValue = 0x12`; `0xFF` = busy) until it returns the
  length, then read the payload (`wValue = 0x13`).
- **Firmware/ping** (`podhd_dev_start`): `wValue = 0x11` returns a 3-byte firmware
  version; the startup sequence ends with a standard `SET_FEATURE`.
- The serial number lives at address `0x80D0` (u32 LE).

The POD X3 startup sequence is, per the kernel comments, not required for the
audio or bulk interfaces to work, so the daemon does not perform it.

## References

- Linux: `sound/usb/line6/{driver,pcm,capture,playback,podhd}.c`
- POD X3 manual, "Setting Up For USB Recording" (channel assignments)
