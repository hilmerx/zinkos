# Zinkos — System Overview

## What Is It?

Zinkos is a low-latency Wi-Fi audio streaming system. Select "Zinkos" as output in macOS Sound settings and hear audio through a DAC connected to a Linux machine on the same network. End-to-end latency: **~25ms** — low enough for video sync and casual music listening, far below AirPlay (~2 seconds).

```
 MacBook Pro                        Wi-Fi                  Thinkpad (Linux)
┌──────────────-───┐              ┌─────────┐            ┌───────-───────────┐
│  Any app audio   │              │         │            │  UDP recv thread  │
│       │          │              │  5ms    │            │       │           │
│       ▼          │              │  UDP    │            │       ▼           │
│  CoreAudio       │──────────────│ packets │───────────▶│  Ring buffer      │
│  (mixes all apps)│              │  976B   │            │       │           │
│       │          │              │  each   │            │       ▼           │
│       ▼          │              │         │            │  Jitter buffer    │
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
| **Zinkos** | **~25ms** | **Lossless PCM** | None (raw S16LE) | **Yes** | Mac → Linux |
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
  │             │  ╔═ Zinkos ═╗         │             │             │
  │             │  ╚══════════╝         │             │             │
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
| **mDNS / Bonjour** | Multicast DNS — a protocol for discovering services on a local network without a central server. Apple's implementation is called Bonjour. Devices announce "I'm here, I offer this service" and others can find them automatically. |

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
- Reads config (receiver IP, port) from macOS preferences plist
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
┌────────────────────────────────────────────────────┐
│  Rust Engine (runs inside coreaudiod)              │
│                                                    │
│  ┌──────────┐   ┌────────────┐   ┌──────────────┐  │
│  │ Ring     │──▶│ Packetizer │──▶│ UDP Sender   │  │
│  │ Buffer   │   │            │   │              │  │
│  │ (SPSC)   │   │ 240 frames │   │ SO_SNDBUF    │  │
│  │ 4800     │   │ + 16B hdr  │   │ DSCP EF      │  │
│  │ frames   │   │ = 976B pkt │   │ (QoS marking)│  │
│  └──────────┘   └────────────┘   └──────────────┘  │
│       ▲                                            │
│       │ write_frames() — RT-safe, lock-free        │
│       │                                            │
│  ┌─────────────-─┐  ┌─────────────────┐            │
│  │ State Machine │  │ Drift Estimator │            │
│  │ (atomic)      │  │ (fill monitor)  │            │
│  └─────────────-─┘  └─────────────────┘            │
└──────────────────────────────────────────────────-─┘
```

- **Ring Buffer** — Lock-free single-producer/single-consumer (`rtrb` crate). The CoreAudio RT thread writes, the network thread reads. Zero contention.
- **Packetizer** — Reads 240 frames (5ms), prepends a 16-byte header (sequence number, frame count, timestamp), outputs a 976-byte UDP packet.
- **UDP Sender** — Sends packets with DSCP Expedited Forwarding marking for QoS on managed networks. 64KB send buffer.
- **State Machine** — Atomic state transitions (Stopped/Starting/Running/Stopping). The RT thread checks `is_accepting_audio()` with a single atomic load.
- **Drift Estimator** — Monitors ring buffer fill level over a sliding window. Computes clock drift in PPM via linear regression. Currently logging-only; could feed a future resampler.

## UDP Packet Format

```
Bytes 0–3:    seq (u32 LE)         — packet sequence number
Bytes 4–7:    frames (u32 LE)      — frame count (typically 240)
Bytes 8–15:   timestamp_ns (u64 LE)— sender clock, for jitter measurement
Bytes 16–975: PCM data             — 240 frames x 4 bytes = 960 bytes
                                      interleaved S16_LE [L, R, L, R, ...]
─────────────
Total: 976 bytes (well under 1500-byte MTU — no fragmentation)
```

## Receiver (C, ~200 lines)

A standalone C program running on the Thinkpad. Two threads:

**Receive thread (RT priority 70):**
- `recv()` on UDP port 4010
- Strip 16-byte header
- Write PCM to lock-free ring buffer

**Playback thread (RT priority 60):**
1. Wait for ring buffer to fill to 15ms (jitter absorption)
2. Pre-fill ALSA buffer
3. Loop: read from ring buffer → `snd_pcm_writei()` → ALSA → DAC
4. On underrun: zero-fill + ALSA recovery

**ALSA configuration:**
```
Period size:      240 frames (5ms)
Periods:          3
Buffer:           720 frames (15ms)
Start threshold:  240 frames (1 period = 5ms)
Format:           S16_LE, 48kHz, stereo
```

Runs as a systemd service (`zinkos-rx.service`) — starts on boot, auto-restarts on failure.

## Latency Breakdown

```
┌───────────────────────────────────────────────────────────┐
│                    ~25ms end-to-end                       │
├──────────────────┬──────────┬─────────────────────────────┤
│   Sender ~13ms   │ Net ~2ms │      Receiver ~10ms         │
├──────────────────┼──────────┼─────────────────────────────┤
│ IO buf    10.7ms │ Wi-Fi    │ Jitter fill        15ms     │
│ Pacer avg  2.5ms │  ~2ms    │ ALSA start          5ms     │
│ UDP send  <0.1ms │          │ DAC                 1ms     │
│                  │          │ (jitter+ALSA overlap ~11ms) │
└──────────────────┴──────────┴─────────────────────────────┘
```

The jitter buffer and ALSA buffer operate concurrently — audio flows from jitter buffer into ALSA continuously, so their latencies partially overlap.

| Stage | Latency | Notes |
|-------|---------|-------|
| CoreAudio IO buffer | 10.7ms | 512 frames @ 48kHz. OS-controlled. |
| Volume + format conversion | <0.01ms | In-place Float32 → S16LE |
| Ring buffer write | <0.01ms | Lock-free, never blocks |
| Pacer wait | ~2.5ms avg | 5ms tick, audio waits for next tick |
| UDP send | <0.1ms | Single syscall, 976 bytes |
| Wi-Fi (same LAN) | ~2ms | Can spike 10–30ms on congested networks |
| Receiver jitter buffer | ~15ms | Absorbs Wi-Fi jitter |
| ALSA playback start | ~5ms | 1 period start threshold |
| DAC | ~1ms | USB DAC latency |
| **Total** | **~25ms** | Jitter + ALSA partially overlap |

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
zinkos install      # symlink to /usr/local/bin
```

The rebuild flow includes safety checks: bundle validation, coreaudiod health monitoring (PID + CPU), and auto-rollback if the driver causes a crash loop.

## Tech Stack

| Component | Language | Why |
|-----------|----------|-----|
| Driver shim | C++ | Required by CoreAudio AudioServerPlugIn API |
| Engine | Rust | Memory safety, lock-free primitives, testable without macOS |
| Receiver | C | Minimal dependencies on Linux, direct ALSA access |
| CLI tool | Bash | No compilation needed, macOS builtins only |
| Build | CMake + Cargo | CMake for the driver bundle, Cargo for Rust crates |

## Testing

The engine is **100% testable without loading the driver or running CoreAudio**:

```bash
cargo test    # runs all engine tests
```

Tests cover:
- Ring buffer: wrap-around, overrun/underrun, concurrent producer/consumer stress
- Packetizer: wire format, sequence wrapping, partial packets
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

- **Adaptive jitter buffer** — Start at 15ms, dynamically shrink/grow based on observed network jitter. Best of both worlds: low latency on clean Wi-Fi, resilience when congested.
- **Clock drift compensation** — The drift estimator already detects PPM-level clock skew between Mac and receiver. Next step: insert/drop a sample every N frames to stay in sync long-term, eliminating slow underrun drift without large buffers.
- **Multi-room / multi-receiver** — Send to multiple receivers simultaneously (UDP multicast or multiple unicast). Each receiver independently buffers and plays back. Sync between rooms via shared sequence numbers.
- **Receiver auto-discovery** — mDNS/Bonjour service advertisement from the receiver. The Mac driver or CLI tool discovers receivers on the network automatically instead of requiring manual IP configuration.

### Exploring

- **macOS Swift app (menu bar)** — Native SwiftUI menu bar app replacing the CLI for day-to-day use. Shows receiver status, volume slider, latency indicator. Manages driver config via the same plist. Could embed a built-in Bonjour browser to pick receivers from a dropdown instead of typing IPs.
- **Bonjour handshake** — Receivers advertise themselves on the network via mDNS (`_zinkos._udp`). The Mac (driver or Swift app) discovers them automatically. The handshake flow:
  1. Receiver starts and registers a Bonjour service: `_zinkos._udp.local.` with TXT record containing device name, sample rate, port
  2. Mac browses for `_zinkos._udp` services via `NSNetServiceBrowser` (Swift app) or `dns-sd` (CLI)
  3. User picks a receiver — IP and port are resolved automatically
  4. Driver begins streaming. No manual IP configuration needed.
  - On the receiver side: Avahi (Linux mDNS) publishes the service. Single config file, no code changes to the receiver binary.
- **Raspberry Pi 5 as primary receiver** — Currently running on a Thinkpad. The Pi 5 with a USB or I2S DAC is the original target hardware. Needs ALSA tuning for the Pi's USB audio stack.
- **FEC (Forward Error Correction)** — Send redundant packets so the receiver can reconstruct lost ones without retransmission. Adds ~1 packet of latency but eliminates glitches from occasional Wi-Fi packet loss.
- **Opus compression** — Optional low-bitrate mode for weaker networks. Opus at 256kbps adds ~5ms encoding latency but reduces bandwidth from ~768kbps to ~256kbps. Would be opt-in; lossless PCM remains the default.
- **iOS companion app** — iOS doesn't allow system audio capture, but a dedicated app could play audio and simultaneously stream it via UDP to the receiver. Limited to in-app audio only.
- **ALSA mmap (direct DMA)** — Bypass kernel buffering on the receiver by writing directly to the DMA buffer. Could save ~5ms but significantly more complex.
- **Smaller IO buffer** — Request 256 frames instead of 512 from CoreAudio. Would halve sender-side buffering latency to ~5.3ms. CoreAudio may or may not honor the request.
