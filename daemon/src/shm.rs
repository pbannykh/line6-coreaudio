//! Shared memory between the daemon and the CoreAudio HAL plugin.
//!
//! Rust mirror of `shared/line6_shm.h`. The layout must match the C struct byte
//! for byte; change both together. The daemon is the sole producer.

use crate::devices::Device;
use std::ffi::CString;
use std::sync::atomic::{AtomicU64, Ordering};

pub const SHM_NAME: &str = "/line6_audio";
pub const MAGIC: u32 = 0x4C49_4E36; // 'LIN6'
pub const VERSION: u32 = 1;
pub const SAMPLE_RATE: u32 = 48_000;
pub const MAX_CHANNELS: usize = 8;
pub const NAME_LEN: usize = 32;
pub const RING_FRAMES: u64 = 65_536; // power of two
pub const RING_MASK: u64 = RING_FRAMES - 1;
pub const RING_SAMPLES: usize = (RING_FRAMES as usize) * MAX_CHANNELS;

/// Mirror of `line6_shm_t`. Accessed only through the mmap'd pointer.
#[repr(C)]
pub struct Shm {
    pub magic: u32,
    pub version: u32,
    pub sample_rate: u32,
    pub channels: u32,
    pub ring_frames: u32,
    pub latency_frames: u32,
    pub name: [u8; NAME_LEN],
    pub write_frames: AtomicU64,
    pub ring: [f32; RING_SAMPLES],
}

const _: () = assert!(std::mem::size_of::<Shm>() == 2_097_216);

/// Creates (or re-opens) the shared segment, maps it, and fills the header for
/// `device`. The segment is persistent (never unlinked) so the daemon can be
/// restarted without the plugin needing to reattach. Returns a pointer valid
/// for the life of the process.
pub fn create(device: &Device, latency_frames: u32) -> Result<*mut Shm, String> {
    let name = CString::new(SHM_NAME).unwrap();
    let size = std::mem::size_of::<Shm>();
    unsafe {
        let fd = libc::shm_open(name.as_ptr(), libc::O_CREAT | libc::O_RDWR, 0o666);
        if fd < 0 {
            return Err(format!("shm_open({SHM_NAME}) failed (errno {})", errno()));
        }
        // ftruncate sizes the segment on first creation only; on reuse it fails
        // with EINVAL (the object already has the right size) which we ignore.
        let _ = libc::ftruncate(fd, size as libc::off_t);
        let p = libc::mmap(
            std::ptr::null_mut(),
            size,
            libc::PROT_READ | libc::PROT_WRITE,
            libc::MAP_SHARED,
            fd,
            0,
        );
        libc::close(fd);
        if p == libc::MAP_FAILED {
            return Err(format!("mmap failed (errno {})", errno()));
        }
        let shm = p as *mut Shm;
        (*shm).magic = MAGIC;
        (*shm).version = VERSION;
        (*shm).sample_rate = SAMPLE_RATE;
        (*shm).channels = device.capture_channels as u32;
        (*shm).ring_frames = RING_FRAMES as u32;
        (*shm).latency_frames = latency_frames;
        write_name(shm, device.name);
        (*shm).write_frames.store(0, Ordering::Release);
        Ok(shm)
    }
}

unsafe fn write_name(shm: *mut Shm, name: &str) {
    let dst = &mut (*shm).name;
    *dst = [0u8; NAME_LEN];
    let bytes = name.as_bytes();
    let n = bytes.len().min(NAME_LEN - 1);
    dst[..n].copy_from_slice(&bytes[..n]);
}

/// Writes whole interleaved frames of S24_3LE into the ring as Float32.
/// `s24` must be a multiple of `channels * 3` bytes.
pub unsafe fn push_frames(shm: *mut Shm, channels: usize, s24: &[u8]) {
    let frame_bytes = channels * 3;
    let frames = s24.len() / frame_bytes;
    if frames == 0 {
        return;
    }
    let mut w = (*shm).write_frames.load(Ordering::Relaxed);
    let ring = (*shm).ring.as_mut_ptr();
    for f in 0..frames {
        let base = f * frame_bytes;
        let slot = ((w & RING_MASK) as usize) * channels;
        for ch in 0..channels {
            let o = base + ch * 3;
            *ring.add(slot + ch) = s24_to_f32(s24[o], s24[o + 1], s24[o + 2]);
        }
        w += 1;
    }
    (*shm).write_frames.store(w, Ordering::Release);
}

/// Writes `frames` of a 440 Hz sine on every channel. Diagnostic alternative to
/// real capture (used by `--sine` and the plugin self-test).
pub unsafe fn push_sine(shm: *mut Shm, channels: usize, frames: usize, phase: &mut f64) {
    const STEP: f64 = 2.0 * std::f64::consts::PI * 440.0 / 48_000.0;
    let mut w = (*shm).write_frames.load(Ordering::Relaxed);
    let ring = (*shm).ring.as_mut_ptr();
    for _ in 0..frames {
        let v = (phase.sin() * 0.25) as f32;
        *phase += STEP;
        if *phase > 2.0 * std::f64::consts::PI {
            *phase -= 2.0 * std::f64::consts::PI;
        }
        let slot = ((w & RING_MASK) as usize) * channels;
        for ch in 0..channels {
            *ring.add(slot + ch) = v;
        }
        w += 1;
    }
    (*shm).write_frames.store(w, Ordering::Release);
}

/// 3 bytes S24_3LE -> f32 in [-1, 1).
#[inline]
fn s24_to_f32(b0: u8, b1: u8, b2: u8) -> f32 {
    let v = (b0 as i32) | ((b1 as i32) << 8) | ((b2 as i32) << 16);
    let v = (v << 8) >> 8; // sign-extend from 24 bits
    v as f32 / 8_388_608.0 // 2^23
}

fn errno() -> i32 {
    std::io::Error::last_os_error().raw_os_error().unwrap_or(0)
}
