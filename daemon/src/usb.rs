//! Isochronous USB capture engine (raw libusb).
//!
//! `nusb` does not support isochronous transfers on macOS, so this uses the raw
//! `libusb1-sys` FFI. The flow mirrors the Linux `sound/usb/line6` driver:
//! claim the audio interface at its active alternate setting, run isochronous IN
//! transfers (capture), and run isochronous OUT transfers carrying silence. The
//! OUT stream is mandatory: the device clocks capture off it (IN_NEEDS_OUT).

#![allow(unsafe_op_in_unsafe_fn)]

use crate::devices::Device;
use crate::shm;
use libusb1_sys as usb;
use std::os::raw::{c_int, c_uint, c_void};
use std::ptr::{addr_of_mut, null_mut};
use std::time::Instant;
use usb::constants::{LIBUSB_TRANSFER_COMPLETED, LIBUSB_TRANSFER_TYPE_ISOCHRONOUS};

/// Packets per transfer. Batching keeps the callback rate sane; the on-wire
/// packet stream is unchanged.
const ISO_PACKETS: usize = 8;
/// Transfers in flight per direction.
const ISO_BUFFERS: usize = 8;
/// Upper bound on frames in one IN packet, for buffer sizing.
const MAX_FRAMES_PER_IN_PKT: usize = 8;
/// Frames per OUT silence packet (48000 / 8000 microframes).
const OUT_FRAMES_PER_PKT: usize = 6;

// Raise this thread's QoS so the scheduler does not preempt isochronous
// processing under load (otherwise frames arrive in bursts -> underruns).
unsafe extern "C" {
    fn pthread_set_qos_class_self_np(qos_class: u32, relative_priority: i32) -> i32;
}
const QOS_CLASS_USER_INTERACTIVE: u32 = 0x21;

pub fn boost_thread_priority() {
    unsafe {
        pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    }
}

/// Per-stream state shared by all transfers. Single-threaded access: libusb
/// callbacks run on our thread inside `handle_events`.
pub struct StreamState {
    pub running: bool,
    in_flight: i32,
    shm: *mut shm::Shm,
    channels: usize,
    frame_bytes: usize,
    in_pkt_len: usize,
    sine: bool,
    sine_phase: f64,
    pub iso_errors: u64,
    pub unaligned_packets: u64,
}

impl StreamState {
    pub fn new(shm: *mut shm::Shm, device: &Device, sine: bool) -> Self {
        StreamState {
            running: true,
            in_flight: 0,
            shm,
            channels: device.capture_channels as usize,
            frame_bytes: device.capture_frame_bytes(),
            in_pkt_len: device.capture_frame_bytes() * MAX_FRAMES_PER_IN_PKT,
            sine,
            sine_phase: 0.0,
            iso_errors: 0,
            unaligned_packets: 0,
        }
    }
}

extern "system" fn on_in(t: *mut usb::libusb_transfer) {
    unsafe {
        let st = &mut *((*t).user_data as *mut StreamState);
        let descs = addr_of_mut!((*t).iso_packet_desc) as *mut usb::libusb_iso_packet_descriptor;
        let base = (*t).buffer;
        for i in 0..(*t).num_iso_packets as isize {
            let d = &*descs.offset(i);
            if d.status != LIBUSB_TRANSFER_COMPLETED {
                st.iso_errors += 1;
            }
            let alen = d.actual_length as usize;
            if alen == 0 {
                continue;
            }
            // Each iso packet starts on a frame (channel 0) boundary: take whole
            // frames, drop any partial tail. Carrying a tail across packets would
            // shift the boundary and rotate the channels permanently.
            if alen % st.frame_bytes != 0 {
                st.unaligned_packets += 1;
            }
            let whole = alen - (alen % st.frame_bytes);
            if whole > 0 {
                let slice = std::slice::from_raw_parts(base.offset(i * st.in_pkt_len as isize), whole);
                if st.sine {
                    shm::push_sine(st.shm, st.channels, whole / st.frame_bytes, &mut st.sine_phase);
                } else {
                    shm::push_frames(st.shm, st.channels, slice);
                }
            }
        }
        if !(st.running && usb::libusb_submit_transfer(t) == 0) {
            st.in_flight -= 1;
        }
    }
}

extern "system" fn on_out(t: *mut usb::libusb_transfer) {
    unsafe {
        // The OUT buffer is zero-initialised and never modified, so this just
        // re-sends silence. Required so the device keeps clocking capture.
        let st = &mut *((*t).user_data as *mut StreamState);
        if !(st.running && usb::libusb_submit_transfer(t) == 0) {
            st.in_flight -= 1;
        }
    }
}

unsafe fn make_iso(
    handle: *mut usb::libusb_device_handle,
    endpoint: u8,
    packets: usize,
    pkt_len: usize,
    cb: usb::libusb_transfer_cb_fn,
    user_data: *mut c_void,
    buf: &mut [u8],
) -> *mut usb::libusb_transfer {
    let t = usb::libusb_alloc_transfer(packets as c_int);
    assert!(!t.is_null(), "libusb_alloc_transfer returned NULL");
    (*t).dev_handle = handle;
    (*t).endpoint = endpoint;
    (*t).transfer_type = LIBUSB_TRANSFER_TYPE_ISOCHRONOUS;
    (*t).timeout = 1000;
    (*t).buffer = buf.as_mut_ptr();
    (*t).length = (packets * pkt_len) as c_int;
    (*t).num_iso_packets = packets as c_int;
    (*t).callback = cb;
    (*t).user_data = user_data;
    (*t).flags = 0;
    let descs = addr_of_mut!((*t).iso_packet_desc) as *mut usb::libusb_iso_packet_descriptor;
    for i in 0..packets as isize {
        (*descs.offset(i)).length = pkt_len as c_uint;
    }
    t
}

/// Owns the libusb context, handle, transfers and their buffers until teardown.
pub struct Engine {
    pub ctx: *mut usb::libusb_context,
    handle: *mut usb::libusb_device_handle,
    interface: c_int,
    transfers: Vec<*mut usb::libusb_transfer>,
    _in_bufs: Vec<Box<[u8]>>,
    _out_bufs: Vec<Box<[u8]>>,
}

/// Opens `device`, claims its audio interface at the active alternate setting,
/// and submits the isochronous IN (capture) and OUT (silence) transfers.
pub unsafe fn start(state: *mut StreamState, device: &Device) -> Result<Engine, String> {
    let mut ctx: *mut usb::libusb_context = null_mut();
    if usb::libusb_init(&mut ctx) != 0 {
        return Err("libusb_init failed".into());
    }

    let handle =
        usb::libusb_open_device_with_vid_pid(ctx, crate::devices::VID_LINE6, device.product_id);
    if handle.is_null() {
        usb::libusb_exit(ctx);
        return Err(format!("could not open {} over USB", device.name));
    }

    let iface = device.audio_interface as c_int;
    let rc = usb::libusb_claim_interface(handle, iface);
    if rc != 0 {
        usb::libusb_close(handle);
        usb::libusb_exit(ctx);
        return Err(format!("claim_interface({iface}): {rc}"));
    }
    let rc = usb::libusb_set_interface_alt_setting(handle, iface, device.alt_setting as c_int);
    if rc != 0 {
        usb::libusb_release_interface(handle, iface);
        usb::libusb_close(handle);
        usb::libusb_exit(ctx);
        return Err(format!("set_alt_setting: {rc}"));
    }

    let in_pkt_len = device.capture_frame_bytes() * MAX_FRAMES_PER_IN_PKT;
    let out_pkt_len = device.playback_frame_bytes() * OUT_FRAMES_PER_PKT;
    let ud = state as *mut c_void;

    let mut in_bufs: Vec<Box<[u8]>> = (0..ISO_BUFFERS)
        .map(|_| vec![0u8; ISO_PACKETS * in_pkt_len].into_boxed_slice())
        .collect();
    let mut out_bufs: Vec<Box<[u8]>> = (0..ISO_BUFFERS)
        .map(|_| vec![0u8; ISO_PACKETS * out_pkt_len].into_boxed_slice())
        .collect();

    let mut transfers: Vec<*mut usb::libusb_transfer> = Vec::with_capacity(ISO_BUFFERS * 2);
    for b in in_bufs.iter_mut() {
        transfers.push(make_iso(handle, device.ep_audio_in, ISO_PACKETS, in_pkt_len, on_in, ud, b));
    }
    for b in out_bufs.iter_mut() {
        transfers.push(make_iso(handle, device.ep_audio_out, ISO_PACKETS, out_pkt_len, on_out, ud, b));
    }

    for &t in &transfers {
        if usb::libusb_submit_transfer(t) == 0 {
            (*state).in_flight += 1;
        }
    }
    if (*state).in_flight == 0 {
        for &t in &transfers {
            usb::libusb_free_transfer(t);
        }
        usb::libusb_release_interface(handle, iface);
        usb::libusb_close(handle);
        usb::libusb_exit(ctx);
        return Err("no transfer started".into());
    }

    Ok(Engine { ctx, handle, interface: iface, transfers, _in_bufs: in_bufs, _out_bufs: out_bufs })
}

/// Services USB events for up to `timeout` (blocks at most that long).
pub unsafe fn handle_events(eng: &Engine, timeout_ms: i64) {
    let tv = libc::timeval {
        tv_sec: 0,
        tv_usec: (timeout_ms * 1000) as libc::suseconds_t,
    };
    usb::libusb_handle_events_timeout(eng.ctx, &tv as *const _ as *const _);
}

/// Cancels transfers, drains, and releases all resources.
pub unsafe fn stop(eng: Engine, state: *mut StreamState) {
    (*state).running = false;
    for &t in &eng.transfers {
        usb::libusb_cancel_transfer(t);
    }
    let drain = Instant::now();
    while (*state).in_flight > 0 && drain.elapsed().as_secs_f64() < 2.0 {
        handle_events(&eng, 100);
    }
    for &t in &eng.transfers {
        usb::libusb_free_transfer(t);
    }
    usb::libusb_release_interface(eng.handle, eng.interface);
    usb::libusb_close(eng.handle);
    usb::libusb_exit(eng.ctx);
}
