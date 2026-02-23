# Zinkos — System Overview

## What Is It?

Zinkos is a low-latency Wi-Fi audio streaming system. Select "Zinkos" as output in macOS Sound settings and hear audio through a DAC connected to a Linux machine on the same network. End-to-end latency: **~9ms tuned / ~25ms default** — low enough for video sync, casual gaming, and music listening, far below AirPlay (~2 seconds).

```
 MacBook Pro                        Wi-Fi                  Linux receiver
┌──────────────-───┐              ┌─────────┐            ┌───────-───────────┐
│  Any app audio   │              │         │            │  UDP recv thread  │
│       │          │              │  UDP    │            │       │           │
│       ▼          │              │ packets │            │       ▼           │
│  CoreAudio       │──────────────│ (size   │───────────▶│  Ring buffer      │
│  (mixes all apps)│              │ depends │            │       │           │
│       │          │              │ on      │            │       ▼           │
│       ▼          │              │ period) │            │  Jitter buffer    │
│  Zinkos driver   │              └─────────┘            │       │           │
│  (volume, encode)│                                     │       ▼           │
│       │          │                                     │  ALSA → DAC       │
│       ▼          │                                     │       │           │
│  Rust engine     │                                     │       ▼           │
│  (ring buf, UDP) │                                     │  🔊 Speaker       │
└──-─────────────-─┘                                     └──────────────────-┘
```

## Comparison With Alternatives

| | Latency | Quality | Codec | Open Source | Cross-platform |
|---|---|---|---|---|---|
| **Zinkos** | **~9ms (tuned) / ~25ms (default)** | **Lossless PCM** | None (raw S16LE) | **Yes** | Mac → Linux |
| AirPlay 2 | ~2000ms | Lossless (ALAC) | ALAC/AAC | No | Apple only |
| Bluetooth A2DP (SBC) | ~150–200ms | Lossy | SBC | Spec open, stacks vary | Universal |
| Bluetooth aptX Low Latency | ~40ms | Lossy | aptX LL | No (Qualcomm) | Needs aptX hardware |
| Bluetooth LE Audio (LC3) | ~20–30ms | Lossy | LC3 | Spec open | New hardware only |
| Sonos (Wi-Fi) | ~75ms (bonded) | Lossless | Proprietary | No | Multi-room |
| Snapcast | ~30–50ms | Lossless PCM | None | Yes | Linux/Mac/Android |
| PulseAudio TCP | ~50–100ms | Lossless PCM | None | Yes | Linux ↔ Linux |
| JACK NetDriver | ~10–20ms | Lossless PCM | None | Yes | Linux (pro audio) |
| Dante (AES67) | ~1–4ms | Lossless PCM | None | No (Audinate) | Pro AV hardware |
| AVB/MILAN | ~2ms | Lossless PCM | None | Spec open | Pro AV hardware |

### Where Zinkos Fits

```
Latency scale (log):

  1ms          10ms         25ms      100ms        1000ms        2000ms
  │             │            │          │             │             │
  ├─ Dante ─────┤            │          │             │             │
  ├─ AVB ───────┤            │          │             │             │
  │        ├─ JACK ──┤       │          │             │             │
  │       ╔═ Zinkos ═══════════╗        │             │             │
  │       ╚═ (tuned → default) ╝        │             │             │
  │             ├── BT LE Audio ─┤      │             │             │
  │             │    ├── aptX LL ┤      │             │             │
  │             │            ├ Snapcast ┤             │             │
  │             │            │  ├── PulseAudio TCP ───┤             │
  │             │            │          ├── Bluetooth A2DP ─────────┤
  │             │            │          │ ├── Sonos ──┤             │
  │             │            │          │             │  ├─ AirPlay ┤
  │             │            │          │             │             │
  wired pro    network pro  Zinkos    consumer      streaming    buffered
```

Zinkos sits in the gap between expensive pro-AV solutions (Dante, AVB) and consumer wireless (AirPlay, Bluetooth). No special hardware, no licensing, no proprietary protocols — just UDP on a standard Wi-Fi network.

## Audio Format (Fixed, No Negotiation)

```
Sample rate:    48,000 Hz
Channels:       2 (stereo, interleaved)
Bit depth:      16-bit signed little-endian (S16_LE)
Bytes/frame:    4 (2 channels x 2 bytes)
```

Chosen for simplicity. One format everywhere — no codec negotiation, no resampling, no conversion overhead.

## Glossary

| Term | What It Is |
|------|-----------|
| **CoreAudio** | macOS system audio framework. Manages all audio devices, mixes app audio, calls driver callbacks. |
| **AudioServerPlugIn** | Apple's API for writing virtual audio drivers. The only way to appear as a system sound output device. Must be C/C++ — Apple does not provide a Swift or Rust interface. |
| **coreaudiod** | The macOS daemon (background process) that hosts all audio drivers. Zinkos runs inside this process. If it crashes, all system audio stops. |
| **Ring buffer** | A fixed-size circular array shared between two threads. One thread writes, the other reads. When the write pointer reaches the end, it wraps to the beginning. Used to decouple the audio thread (strict timing) from the network thread (variable timing). |
| **SPSC** | Single-Producer Single-Consumer — a ring buffer design where exactly one thread writes and one thread reads. This allows a lock-free implementation using only atomic variables, with zero contention. |
| **Lock-free** | A programming technique where threads coordinate using atomic CPU instructions instead of mutexes/locks. Critical for real-time audio — a lock could cause the audio thread to wait, producing an audible glitch. |
| **RT thread** | Real-Time thread. CoreAudio's audio callback runs on a thread with strict timing guarantees. It must return within microseconds. Any blocking (locks, allocations, disk I/O) causes audible dropouts across ALL system audio, not just Zinkos. |
| **Jitter buffer** | A small buffer on the receiver that absorbs timing variation in network packet arrival. Wi-Fi packets might arrive 1ms apart or 15ms apart — the jitter buffer smooths this out by accumulating audio before starting playback. Tradeoff: more buffering = more resilience but higher latency. |
| **UDP** | User Datagram Protocol. A network protocol that sends packets without delivery guarantees — no retransmission, no ordering, no connection setup. Ideal for real-time audio: a late packet is worse than a lost packet. TCP's retransmission would add unpredictable latency. |
| **ALSA** | Advanced Linux Sound Architecture. The Linux kernel's audio subsystem. Provides direct access to audio hardware with configurable buffer sizes. The receiver writes PCM samples to ALSA, which feeds the DAC. |
| **DAC** | Digital-to-Analog Converter. The hardware that converts digital audio samples into an electrical signal that drives speakers. Can be USB, I2S, or built into the sound card. |
| **S16_LE** | Signed 16-bit Little-Endian — a PCM audio sample format. Each sample is a 16-bit integer (-32768 to +32767). "Little-Endian" means the least significant byte comes first in memory. Standard format for CD-quality audio. |
| **PCM** | Pulse-Code Modulation. Raw, uncompressed digital audio — just a stream of amplitude values sampled at a fixed rate. No codec, no compression, no quality loss. |
| **FFI** | Foreign Function Interface. The mechanism for calling functions written in one language from another. Zinkos uses Rust's `extern "C"` FFI to expose functions that C++ can call, with a plain C calling convention. |
| **cdylib** | A Rust compilation target that produces a C-compatible dynamic library (.dylib on macOS). Looks like a normal C library to the linker — the caller doesn't need to know it's written in Rust. |
| **DSCP / QoS** | Differentiated Services Code Point — a flag in the IP packet header that tells network equipment to prioritize this traffic. Zinkos marks packets as "Expedited Forwarding" (EF), the highest priority class. Helps on managed networks; most home routers ignore it. |
| **PPM** | Parts Per Million. Used to measure clock drift. A 50 PPM drift between two 48kHz clocks means one produces 48000 samples/second while the other produces 48002.4 — a difference of 2.4 frames/second. |
| **mDNS / Bonjour** | Multicast DNS — a protocol for discovering services on a local network without a central server. Apple's implementation is called Bonjour. Devices announce "I'm here, I offer this service" and others can find them automatically. Zinkos receivers advertise via `_zinkos._udp` and the macOS setup app discovers them automatically. |

## Why Rust + C++?

The system is split across two languages for practical reasons:

**C++ (driver shim, ~500 lines):** Apple's AudioServerPlugIn API is a C/C++ COM-style interface. There is no Swift or Rust binding. The driver must implement specific C function pointers in a vtable and link against CoreAudio frameworks. This part _must_ be C or C++. It's kept as thin as possible — just translates CoreAudio callbacks into engine calls.

**Rust (engine, ~1500 lines):** All real logic — ring buffer, packetizer, UDP networking, state machine, drift estimation. Rust was chosen because:

1. **Memory safety without garbage collection.** Audio code that runs inside a system daemon cannot afford use-after-free bugs or buffer overflows. A crash in the engine takes down coreaudiod and kills ALL system audio.
2. **Lock-free primitives.** Rust's ownership model enforces at compile time that the ring buffer's producer and consumer are on separate threads with no shared mutable state. Data races are caught by the compiler, not by debugging audio glitches at 3am.
3. **Testable without macOS.** `cargo test` runs all engine logic without CoreAudio, without loading a driver, without being root. The C++ shim is ~500 lines of untestable glue; the Rust engine is ~1500 lines of fully tested logic.
4. **cdylib FFI.** Rust compiles to a `.dylib` with `extern "C"` functions. The C++ driver calls it like any C library — no Rust runtime needed on the caller side.

**C (receiver, ~200 lines):** Minimal dependencies on the Linux receiver. Direct ALSA access, pthreads for real-time scheduling. No allocator, no runtime. Could be Rust, but C keeps the receiver a single self-contained file with zero build dependencies beyond `libasound`.

```
┌─────────────────────────────────────────────────────-┐
│ Why not all C++?                                     │
│   → No memory safety. A buffer overflow in the       │
│     engine crashes coreaudiod = all Mac audio dies.  │
│                                                      │
│ Why not all Rust?                                    │
│   → Apple's AudioServerPlugIn API requires C/C++.    │
│     No Rust bindings exist for the HAL plugin API.   │
│                                                      │
│ Solution: C++ for the 500 lines Apple forces,        │
│           Rust for the 1500 lines of real logic.     │
└─────────────────────────────────────────────────────-┘
```

## The Two Layers

### Layer 1 — Driver Shim (C++, ~500 lines)

A macOS CoreAudio AudioServerPlugIn — the only way to appear as a system audio output device. Implements a COM-like interface that CoreAudio expects.

**What it does:**
- Registers "Zinkos" as an output device in macOS Sound preferences
- Receives mixed system audio from CoreAudio as Float32 samples
- Applies volume (quadratic perceptual curve) and converts to S16LE
- Hands samples to the Rust engine via FFI
- Reads config (receiver IP, port, frames per packet) from macOS preferences plist
- Persists volume/mute state to survive coreaudiod restarts

**What it explicitly does NOT do:**
- Any networking
- Any buffering logic
- Any real-time decisions

The driver runs inside `coreaudiod` (a system daemon). Its IO callback is on a real-time thread — no allocations, no locks, no syscalls allowed. Write to the ring buffer and get out.

### Layer 2 — Engine (Rust, ~1500 lines)

All actual logic. Compiled as a `cdylib` (C-compatible shared library) and called from the driver via `extern "C"` FFI.

**Components:**

```
┌──────────────────────────────────────────────────────────┐
│  Rust Engine (runs inside coreaudiod)                    │
│                                                          │
│  ┌──────────┐   ┌──────────────┐   ┌──────────────┐     │
│  │ Ring     │──▶│ Packetizer   │──▶│ UDP Sender   │     │
│  │ Buffer   │   │              │   │              │     │
│  │ (SPSC)   │   │ N frames     │   │ SO_SNDBUF    │     │
│  │ 4800     │   │ + 20B hdr    │   │ DSCP EF      │     │
│  │ frames   │   │ (magic+ver)  │   │ (QoS marking)│     │
│  └──────────┘   └──────────────┘   └──────────────┘     │
│       ▲                                                  │
│       │ write_frames() — RT-safe, lock-free              │
│       │                                                  │
│  ┌─────────────-─┐  ┌─────────────────┐                  │
│  │ State Machine │  │ Drift Estimator │                  │
│  │ (atomic)      │  │ (fill monitor)  │                  │
│  └─────────────-─┘  └─────────────────┘                  │
└──────────────────────────────────────────────────────────┘
```

- **Ring Buffer** — Lock-free single-producer/single-consumer (`rtrb` crate). The CoreAudio RT thread writes, the network thread reads. Zero contention.
- **Packetizer** — Reads N frames (configurable via `FramesPerPacket`, default 240 = 5ms), prepends a 20-byte header (magic, protocol version, flags, sequence number, frame count, timestamp), outputs a UDP packet.
- **UDP Sender** — Sends packets with DSCP Expedited Forwarding marking for QoS on managed networks. 64KB send buffer.
- **State Machine** — Atomic state transitions (Stopped/Starting/Running/Stopping). The RT thread checks `is_accepting_audio()` with a single atomic load.
- **Drift Estimator** — Monitors ring buffer fill level over a sliding window. Computes clock drift in PPM via linear regression. Currently logging-only; could feed a future resampler.

## UDP Packet Format

```
Bytes 0–1:    magic (u16 LE)       — 0x5A4B ("ZK") — identifies Zinkos packets
Bytes 2:      version (u8)         — protocol version (currently 1)
Bytes 3:      flags (u8)           — reserved (0)
Bytes 4–7:    seq (u32 LE)         — packet sequence number
Bytes 8–11:   frames (u32 LE)      — frame count in this packet
Bytes 12–19:  timestamp_ns (u64 LE)— nanoseconds since stream start
Bytes 20–N:   PCM data             — frames × 4 bytes (interleaved S16_LE [L, R, L, R, ...])
─────────────
Header: 20 bytes (fixed)
Payload: frames_per_packet × 4 bytes
Default (240 frames): 20 + 960 = 980 bytes
Tuned (100 frames):   20 + 400 = 420 bytes
All well under 1500-byte MTU — no fragmentation.
```

The magic bytes and protocol version enable receivers to detect protocol mismatches. Receivers advertise their supported protocol version via mDNS TXT records, and the macOS setup app shows compatibility status.

## Receiver (C, ~200 lines)

A standalone C program running on the Linux receiver. Two threads:

**Receive thread (RT priority 70):**
- `recv()` on UDP port 4010
- Validate 20-byte header (magic bytes, version)
- Write PCM to lock-free ring buffer

**Playback thread (RT priority 60):**
1. Wait for ring buffer to fill to configured start-fill (default 15ms, tunable)
2. Pre-fill ALSA buffer
3. Loop: read from ring buffer → `snd_pcm_writei()` → ALSA → DAC
4. On underrun: zero-fill + ALSA recovery

**ALSA configuration (configurable via install.sh):**
```
Period size:      configurable (default 240 frames = 5ms, tuned: 100 frames = ~2ms)
Periods:          3
Buffer:           period × 3 (default 720 = 15ms, tuned: 300 = 6.25ms)
Start threshold:  period × 2
Format:           S16_LE, 48kHz, stereo
```

The receiver reads `frames_per_packet` from the packet header, not from a compile-time constant. This means it adapts automatically when the sender changes its period setting — no receiver restart needed for that change. The ALSA period and start-fill are set during `install.sh` and should match the sender's period for optimal results.

Runs as a systemd service (`zinkos-rx.service`) — starts on boot, auto-restarts on failure. Advertises itself via Avahi/mDNS (`_zinkos._udp`) for automatic discovery by the macOS setup app.

## Latency Breakdown

```
┌──────────────────────────────────────────────────────────────┐
│              ~9ms tuned / ~25ms default                       │
├──────────────────┬──────────┬────────────────────────────────┤
│   Sender ~5ms    │ Net ~2ms │      Receiver ~3ms (tuned)     │
├──────────────────┼──────────┼────────────────────────────────┤
│ IO buf     2.7ms │ Wi-Fi    │ Jitter fill         3ms        │
│ Pacer avg  2.0ms │  ~2ms    │ DAC                 1ms        │
│ UDP send  <0.1ms │          │                                │
└──────────────────┴──────────┴────────────────────────────────┘
```

The jitter buffer and ALSA buffer operate concurrently — audio flows from jitter buffer into ALSA continuously, so their latencies partially overlap.

| Stage | Default | Tuned | Notes |
|-------|---------|-------|-------|
| CoreAudio IO buffer | 2.67ms | 2.67ms | 128 frames @ 48kHz |
| Volume + format conversion | <0.01ms | <0.01ms | In-place Float32 → S16LE |
| Ring buffer write | <0.01ms | <0.01ms | Lock-free, never blocks |
| Pacer wait | ~2.5ms avg | ~1ms avg | Default 5ms tick, tuned ~2ms tick |
| UDP send | <0.1ms | <0.1ms | Single syscall |
| Wi-Fi (same LAN) | ~2ms | ~2ms | Can spike on congested networks |
| Receiver start-fill | 15ms | 3ms | Absorbs Wi-Fi jitter |
| DAC | ~1ms | ~1ms | USB DAC latency |
| **Total** | **~25ms** | **~9ms** | |

## Setup App (macOS)

A native SwiftUI app that discovers receivers on the network via Bonjour/mDNS. No manual IP entry needed.

- Discovers `_zinkos._udp` services automatically
- Shows receiver name, hostname, protocol version compatibility
- Configures: receiver IP/hostname, port, latency offset, frames per packet (period)
- Saves to `com.zinkos.driver` plist and restarts coreaudiod
- Stores mDNS hostnames (e.g. `mypi.local`) so config survives DHCP IP changes

```bash
cd sender/app && swift build -c release && .build/release/Zinkos
```

## Real-Time Safety

The most critical constraint: **CoreAudio's IO callback runs on a real-time thread.** If it blocks, audio glitches system-wide — not just Zinkos, ALL audio output on the Mac.

Rules enforced in the driver:
1. **No allocations** — all buffers pre-allocated
2. **No locks** — ring buffer is lock-free (SPSC with atomics)
3. **No syscalls** — no file I/O, no network calls
4. **No blocking** — `write_frames()` drops data if buffer is full rather than waiting

The network thread (non-RT) does all the heavy lifting: reading the ring buffer, building packets, calling `sendto()`.

## CLI Tool

```bash
zinkos status       # show config + driver health
zinkos set ip <IP>  # set receiver IP
zinkos set port <N> # set receiver port
zinkos rebuild      # compile, validate, install, reload
zinkos reload       # restart coreaudiod (picks up config changes)
```

The rebuild flow includes safety checks: bundle validation, coreaudiod health monitoring (PID + CPU), and auto-rollback if the driver causes a crash loop.

## Tech Stack

| Component | Language | Why |
|-----------|----------|-----|
| Driver shim | C++ | Required by CoreAudio AudioServerPlugIn API |
| Engine | Rust | Memory safety, lock-free primitives, testable without macOS |
| Receiver | C | Minimal dependencies on Linux, direct ALSA access |
| Setup app | Swift | Native macOS Bonjour discovery, SwiftUI config UI |
| CLI tool | Bash | No compilation needed, macOS builtins only |
| Build | CMake + Cargo | CMake for the driver bundle, Cargo for Rust crates |

## Testing

The engine is **100% testable without loading the driver or running CoreAudio**:

```bash
cargo test    # runs all engine tests
```

Tests cover:
- Ring buffer: wrap-around, overrun/underrun, concurrent producer/consumer stress
- Packetizer: wire format, sequence wrapping, partial packets, magic/version validation
- State machine: lifecycle transitions, idempotency, concurrent reads
- Drift estimator: stable/drifting clock detection
- UDP sender: localhost round-trip
- Config: constant sanity checks (packet fits MTU, buffer holds 90ms+)

Integration testing without driver loading:
- CLI tool feeds audio into engine, verifies UDP packets on localhost
- Receiver tested with recorded packet streams

What **requires** loading the driver:
- macOS enumerating it as an output device
- Real IO callback timing
- End-to-end latency measurement
- System sleep/wake behavior

## Volume Persistence

Volume state survives coreaudiod restarts (which happen during rebuilds and system events).

- **Storage:** Plain text file at `/Library/Preferences/Audio/com.zinkos.volume`
- **Why not plist/CFPreferences:** coreaudiod runs as `_coreaudiod` user, which is sandboxed from `CFPreferencesSetAppValue`. Direct file I/O to the Audio prefs directory works.
- **Throttling:** Writes at most every 500ms (macOS sends dozens of SetPropertyData calls per second when dragging the slider)
- **Final save:** Forced write on StopIO to capture the last value
- **Volume curve:** Quadratic (`gain = scalar * scalar`) for perceptually linear behavior
- **Default:** 50% if no saved state exists (never blasts full volume on first install)

## Future Features

### Planned

- **Adaptive jitter buffer** — Start at configured fill, dynamically shrink/grow based on observed network jitter. Best of both worlds: low latency on clean Wi-Fi, resilience when congested.
- **Clock drift compensation** — The drift estimator already detects PPM-level clock skew between Mac and receiver. Next step: insert/drop a sample every N frames to stay in sync long-term, eliminating slow underrun drift without large buffers.
- **Multi-room / multi-receiver** — Send to multiple receivers simultaneously (UDP multicast or multiple unicast). Each receiver independently buffers and plays back. Sync between rooms via shared sequence numbers.
- **FEC (Forward Error Correction)** — Send redundant packets so the receiver can reconstruct lost ones without retransmission. Adds ~1 packet of latency but eliminates glitches from occasional Wi-Fi packet loss.

### Exploring

- **Configurable sample rate** — Support 44.1kHz and 96kHz in addition to 48kHz.
- **Raspberry Pi 5 as receiver** — The Pi 5 with a USB or I2S DAC. Needs ALSA tuning for the Pi's USB audio stack.
- **Opus compression** — Optional low-bitrate mode for weaker networks. Opus at 256kbps adds ~5ms encoding latency but reduces bandwidth from ~768kbps to ~256kbps. Would be opt-in; lossless PCM remains the default.
- **iOS companion app** — iOS doesn't allow system audio capture, but a dedicated app could play audio and simultaneously stream it via UDP to the receiver. Limited to in-app audio only.
- **ALSA mmap (direct DMA)** — Bypass kernel buffering on the receiver by writing directly to the DMA buffer. Could save a few ms but significantly more complex.
