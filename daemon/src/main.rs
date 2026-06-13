//! Line 6 USB capture daemon for macOS.
//!
//! Captures audio from a connected Line 6 device over USB (raw libusb) and
//! writes it into a shared-memory ring that the CoreAudio HAL plugin reads.
//!
//! Subcommands:
//!   daemon   capture into shared memory (default); options below
//!   list     show supported devices and what is currently connected
//!
//! `daemon` options:
//!   --latency-ms N        desired plugin buffer depth (default 64)
//!   --exit-when-absent    exit cleanly when no device is connected
//!                         (for launchd "start on connect"; see scripts/)
//!   --sine                write a 440 Hz test tone instead of real capture

mod devices;
mod shm;
mod usb;

use devices::Device;
use std::sync::atomic::{AtomicBool, Ordering};
use std::time::{Duration, Instant};

/// Cleared by SIGINT/SIGTERM to stop the daemon.
static RUNNING: AtomicBool = AtomicBool::new(true);

extern "C" fn on_signal(_sig: i32) {
    RUNNING.store(false, Ordering::SeqCst);
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    let cmd = args.get(1).map(String::as_str).unwrap_or("daemon");
    let result = match cmd {
        "daemon" => run_daemon(
            args.iter().any(|a| a == "--sine"),
            args.iter().any(|a| a == "--exit-when-absent"),
            flag_value(&args, "--latency-ms").unwrap_or(64),
        ),
        "list" => {
            list_devices();
            Ok(())
        }
        other => Err(format!("unknown command '{other}' (use: daemon, list)")),
    };
    if let Err(e) = result {
        eprintln!("error: {e}");
        std::process::exit(1);
    }
}

/// Parses a numeric flag of the form `--flag N`.
fn flag_value(args: &[String], flag: &str) -> Option<u32> {
    args.iter()
        .position(|a| a == flag)
        .and_then(|i| args.get(i + 1))
        .and_then(|v| v.parse().ok())
}

fn list_devices() {
    println!("Supported devices:");
    for d in devices::DEVICES {
        println!(
            "  {:<14} {:04x}:{:04x}  {} in / {} out",
            d.name, devices::VID_LINE6, d.product_id, d.capture_channels, d.playback_channels
        );
    }
    match devices::find_connected() {
        Some(d) => println!("Connected: {} ({:04x})", d.name, d.product_id),
        None => println!("Connected: none"),
    }
}

const WATCHDOG_STALL_S: f64 = 0.5; // no new frames for this long -> reinit engine
const ABSENT_RETRY_S: u64 = 2; // how often to look for a device when none is present
const ABSENT_EXIT_GRACE: u32 = 3; // --exit-when-absent: ~3 * ABSENT_RETRY_S before exit

fn run_daemon(sine: bool, exit_when_absent: bool, latency_ms: u32) -> Result<(), String> {
    let handler = on_signal as extern "C" fn(i32) as libc::sighandler_t;
    unsafe {
        libc::signal(libc::SIGINT, handler);
        libc::signal(libc::SIGTERM, handler);
    }
    usb::boost_thread_priority();

    let latency_frames = latency_ms.clamp(5, 500) * (devices::SAMPLE_RATE_HZ / 1000);
    println!("Line 6 daemon started (latency {latency_ms} ms). Ctrl-C to stop.");

    // `active` holds the running capture: its state (referenced by the USB
    // transfers, so it must outlive the engine) and the engine itself.
    let mut active: Option<(Box<usb::StreamState>, usb::Engine)> = None;
    let mut shm_ptr: *mut shm::Shm = std::ptr::null_mut();
    let mut shm_pid: u16 = 0; // product id the shm header currently describes

    let mut last_wf = 0u64;
    let mut last_progress = Instant::now();
    let mut last_print = Instant::now();
    let mut absent_count = 0u32;
    let mut absent_logged = false;

    while RUNNING.load(Ordering::SeqCst) {
        if active.is_some() {
            // Short borrow to service USB events, then release it so the
            // watchdog below can take() the engine if needed.
            unsafe { usb::handle_events(&active.as_ref().unwrap().1, 100) };

            // Watchdog: if write_frames stops growing the capture has stalled
            // (iso wedged or device unplugged) -> tear the engine down so the
            // next iteration rebuilds it (or exits, if the device is gone).
            let wf = unsafe { (*shm_ptr).write_frames.load(Ordering::Relaxed) };
            let stalled = if wf != last_wf {
                last_wf = wf;
                last_progress = Instant::now();
                false
            } else {
                last_progress.elapsed().as_secs_f64() > WATCHDOG_STALL_S
            };
            if stalled {
                if let Some((state, eng)) = active.take() {
                    let ptr = Box::into_raw(state);
                    unsafe {
                        usb::stop(eng, ptr);
                        drop(Box::from_raw(ptr));
                    }
                }
            }

            if last_print.elapsed().as_secs_f64() >= 2.0 {
                if let Some((s, _)) = &active {
                    println!(
                        "capturing: {wf} frames (~{:.1}s), iso errors {}, unaligned {}",
                        wf as f64 / 48_000.0,
                        s.iso_errors,
                        s.unaligned_packets
                    );
                }
                last_print = Instant::now();
            }
            continue;
        }

        // No capture running: look for a device and (re)start.
        match devices::find_connected() {
            Some(device) => {
                if shm_ptr.is_null() || shm_pid != device.product_id {
                    shm_ptr = shm::create(device, latency_frames)?;
                    shm_pid = device.product_id;
                }
                match start_capture(shm_ptr, device, sine) {
                    Ok(pair) => {
                        println!("{} connected — capturing.", device.name);
                        active = Some(pair);
                        absent_count = 0;
                        absent_logged = false;
                        last_progress = Instant::now();
                        last_wf = unsafe { (*shm_ptr).write_frames.load(Ordering::Relaxed) };
                    }
                    Err(e) => {
                        // Device present but could not be opened (busy / racing
                        // with a previous instance). Keep retrying; do NOT exit.
                        if !absent_logged {
                            println!("{} present but busy ({e}). Retrying...", device.name);
                            absent_logged = true;
                        }
                        absent_count = 0;
                        idle_sleep(ABSENT_RETRY_S);
                    }
                }
            }
            None => {
                if exit_when_absent {
                    absent_count += 1;
                    if absent_count >= ABSENT_EXIT_GRACE {
                        println!("No device connected — exiting (launchd restarts on connect).");
                        break;
                    }
                }
                if !absent_logged {
                    println!("No supported device connected. Waiting...");
                    absent_logged = true;
                }
                idle_sleep(ABSENT_RETRY_S);
            }
        }
    }

    if let Some((state, eng)) = active.take() {
        let ptr = Box::into_raw(state);
        unsafe {
            usb::stop(eng, ptr);
            drop(Box::from_raw(ptr));
        }
    }
    println!("Stopped (shared memory kept for the plugin).");
    Ok(())
}

/// Boxes a fresh stream state and starts the USB engine for `device`.
fn start_capture(
    shm_ptr: *mut shm::Shm,
    device: &'static Device,
    sine: bool,
) -> Result<(Box<usb::StreamState>, usb::Engine), String> {
    let mut state = Box::new(usb::StreamState::new(shm_ptr, device, sine));
    let ptr: *mut usb::StreamState = &mut *state;
    let eng = unsafe { usb::start(ptr, device)? };
    Ok((state, eng))
}

/// Sleeps `secs` in 500 ms slices so Ctrl-C stays responsive.
fn idle_sleep(secs: u64) {
    for _ in 0..secs * 2 {
        if !RUNNING.load(Ordering::SeqCst) {
            break;
        }
        std::thread::sleep(Duration::from_millis(500));
    }
}
