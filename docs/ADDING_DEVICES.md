# Adding a device

The driver is built so that adding another Line 6 USB device is mostly adding one
row to a table. The channel count and model name flow from there through shared
memory to the plugin, so the plugin needs no changes.

This works for devices that use the same isochronous transport (the POD X3 family
and the other devices in the Linux `sound/usb/line6` driver). It is **untested**
beyond POD X3 — treat a new entry as needing verification.

## 1. Find the device's parameters

The easiest source is the Linux kernel driver `sound/usb/line6/`. Look at the
`*_properties_table` for your model (e.g. `podhd_properties_table` in `podhd.c`,
`pod_properties_table` in `pod.c`). You need:

| Field | Linux source |
|-------|--------------|
| product id | `usb_device_id` table |
| `audio_interface` | the interface with the iso endpoints (usually 0) |
| `alt_setting` | `.altsetting` |
| `ep_audio_in` | `.ep_audio_r` |
| `ep_audio_out` | `.ep_audio_w` |
| capture / playback channels | the `*_pcm_properties` `channels_max` |

You can cross-check the product id and endpoints with `system_profiler SPUSBDataType`
or `ioreg -p IOUSB -l` while the device is plugged in.

> Only 48 kHz / S24_3LE devices are supported. The shared-memory ring is sized for
> up to `LINE6_MAX_CHANNELS` (8) capture channels; raise it (in `shared/line6_shm.h`
> and `daemon/src/shm.rs`, keeping the two in sync) if you add a wider device.

## 2. Add a row to the device table

In `daemon/src/devices.rs`, add an entry to `DEVICES`:

```rust
Device {
    name: "POD HD500",
    product_id: 0x414D,
    audio_interface: 0,
    alt_setting: 1,
    ep_audio_in: 0x86,
    ep_audio_out: 0x02,
    capture_channels: 4,
    playback_channels: 2,
},
```

## 3. (On-demand only) add USB launch matching

If you use the on-demand launchd agent, add a matching block for the new product
id in `scripts/launchagent.sh` (the `com.apple.iokit.matching` dictionary), so
launchd starts the daemon when that device is connected. The always-on agent
needs no change.

## 4. Build and test

```sh
cd daemon && cargo build --release
./target/release/line6-daemon list      # should show your device when connected
./target/release/line6-daemon daemon    # capture; watch "capturing: N frames"
```

Verify the channels are in the order you expect (play a signal into one input and
check which captured channel carries it). If channels look rotated, re-check the
framing assumptions in [PROTOCOL.md](PROTOCOL.md).

Then run the plugin's `haltest` gate and install as usual (`scripts/install.sh`).

## Devices that need a startup handshake

Some devices may require the vendor startup sequence (`podhd_dev_start`: a `0x67`
ping plus `SET_FEATURE`) before they stream. POD X3 does not. If a new device does
not start capturing, implementing that sequence over endpoint 0 before claiming the
audio interface is the place to look (see [PROTOCOL.md](PROTOCOL.md#control-protocol-optional)).
