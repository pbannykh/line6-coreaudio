//
//  line6_shm.h
//  IPC contract between the capture daemon (producer) and the CoreAudio HAL
//  plugin (consumer).
//
//  A POSIX shared-memory segment holds a small header plus a lock-free single
//  producer / single consumer ring of interleaved Float32 audio. The daemon
//  captures audio over USB and writes it here; the plugin reads it and hands it
//  to CoreAudio.
//
//  This layout MUST match the Rust mirror in daemon/src/shm.rs byte for byte.
//  Change both sides together.
//

#ifndef LINE6_SHM_H
#define LINE6_SHM_H

#include <stdint.h>
#include <stdatomic.h>

#define LINE6_SHM_NAME     "/line6_audio"  // name passed to shm_open
#define LINE6_SHM_MAGIC    0x4C494E36u      // 'LIN6'
#define LINE6_SHM_VERSION  1u

#define LINE6_SAMPLE_RATE  48000u
#define LINE6_MAX_CHANNELS 8u               // ring is sized for the widest device
#define LINE6_NAME_LEN     32u

// Ring capacity in FRAMES. Power of two for a cheap mask. ~1.37 s at 48 kHz.
#define LINE6_RING_FRAMES  65536u
#define LINE6_RING_MASK    (LINE6_RING_FRAMES - 1u)
#define LINE6_RING_SAMPLES (LINE6_RING_FRAMES * LINE6_MAX_CHANNELS)

// One producer (daemon), one consumer (plugin). Synchronisation is the atomic
// write_frames counter. The header fields up to write_frames are written once
// at startup and keep write_frames on an 8-byte boundary.
typedef struct {
    uint32_t magic;          // LINE6_SHM_MAGIC
    uint32_t version;        // LINE6_SHM_VERSION
    uint32_t sample_rate;    // LINE6_SAMPLE_RATE
    uint32_t channels;       // capture channels of the connected device (<= MAX)
    uint32_t ring_frames;    // LINE6_RING_FRAMES
    uint32_t latency_frames; // desired plugin buffer depth; 0 -> plugin default

    char     name[LINE6_NAME_LEN]; // connected model, e.g. "POD X3 Live" (NUL-term)

    // Total frames written since start (free-running). Write position in the ring
    // = write_frames & LINE6_RING_MASK; the plugin reads at a fixed depth behind.
    _Atomic uint64_t write_frames;

    // Interleaved Float32 in [-1, 1]: ring[(frame & MASK) * channels + ch].
    float ring[LINE6_RING_SAMPLES];
} line6_shm_t;

#define LINE6_SHM_SIZE (sizeof(line6_shm_t))

#endif /* LINE6_SHM_H */
