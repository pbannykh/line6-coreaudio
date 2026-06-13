//! Supported Line 6 USB audio devices.
//!
//! Each entry says how to talk to one device: USB product id, the audio
//! interface and the alternate setting that activates it, the isochronous
//! endpoints, and the channel layout. These values come from the Linux kernel
//! driver `sound/usb/line6/` (its `*_properties_table` entries).
//!
//! To add a device, add a row to [`DEVICES`]. See `docs/ADDING_DEVICES.md`.

use libusb1_sys as usb;
use std::ptr::null_mut;

/// Line 6 USB vendor id (shared by every device).
pub const VID_LINE6: u16 = 0x0E41;

/// Sample format used by every Line 6 device here: 24-bit, packed in 3 bytes.
pub const BYTES_PER_SAMPLE: usize = 3;
pub const SAMPLE_RATE_HZ: u32 = 48_000;

/// Static description of one supported device.
pub struct Device {
    /// Model name reported to CoreAudio.
    pub name: &'static str,
    pub product_id: u16,
    /// Interface carrying the isochronous audio endpoints.
    pub audio_interface: u8,
    /// Alternate setting that activates the audio endpoints.
    pub alt_setting: u8,
    /// Isochronous IN endpoint (capture).
    pub ep_audio_in: u8,
    /// Isochronous OUT endpoint. The daemon only sends silence here, which the
    /// device requires in order to clock the capture stream (LINE6_CAP_IN_NEEDS_OUT).
    pub ep_audio_out: u8,
    /// Capture channels (interleaved S24_3LE @ 48 kHz).
    pub capture_channels: u8,
    /// Playback channels (used only to size the OUT silence packets).
    pub playback_channels: u8,
}

impl Device {
    /// Capture frame size in bytes (channels * 3).
    pub const fn capture_frame_bytes(&self) -> usize {
        self.capture_channels as usize * BYTES_PER_SAMPLE
    }
    /// Playback frame size in bytes (channels * 3).
    pub const fn playback_frame_bytes(&self) -> usize {
        self.playback_channels as usize * BYTES_PER_SAMPLE
    }
}

/// The supported devices.
///
/// Only POD X3 / X3 Live are tested. Other Line 6 USB devices handled by the
/// Linux driver share this transport and can be added once their endpoints and
/// channel counts are confirmed.
pub static DEVICES: &[Device] = &[
    Device {
        name: "POD X3 Live",
        product_id: 0x414B,
        audio_interface: 0,
        alt_setting: 1,
        ep_audio_in: 0x86,
        ep_audio_out: 0x02,
        // main L/R, tone1 L/R, tone2 L/R, dry1, dry2
        capture_channels: 8,
        playback_channels: 2,
    },
    Device {
        name: "POD X3",
        product_id: 0x414A,
        audio_interface: 0,
        alt_setting: 1,
        ep_audio_in: 0x86,
        ep_audio_out: 0x02,
        capture_channels: 8,
        playback_channels: 2,
    },
];

/// Finds the first connected supported device by scanning the USB bus.
/// Returns the matching table entry, or `None` if none is plugged in.
pub fn find_connected() -> Option<&'static Device> {
    unsafe {
        let mut ctx: *mut usb::libusb_context = null_mut();
        if usb::libusb_init(&mut ctx) != 0 {
            return None;
        }
        let mut list: *const *mut usb::libusb_device = null_mut();
        let n = usb::libusb_get_device_list(ctx, &mut list);
        let mut found: Option<&'static Device> = None;
        if n > 0 {
            'scan: for i in 0..n {
                let dev = *list.offset(i);
                let mut desc: usb::libusb_device_descriptor = std::mem::zeroed();
                if usb::libusb_get_device_descriptor(dev, &mut desc) != 0 {
                    continue;
                }
                if desc.idVendor != VID_LINE6 {
                    continue;
                }
                for d in DEVICES {
                    if d.product_id == desc.idProduct {
                        found = Some(d);
                        break 'scan;
                    }
                }
            }
            usb::libusb_free_device_list(list, 1);
        }
        usb::libusb_exit(ctx);
        found
    }
}
