# Zinkos — Low-Latency Wi-Fi Audio Streaming Driver

## Rules

- Never add Co-Authored-By to commit messages
- Never include Claude as a contributor in any form
- Never commit or push unless explicitly asked

## Project Overview

Zinkos is a macOS CoreAudio AudioServerPlugIn driver + Raspberry Pi receiver that streams system audio over Wi-Fi with low latency. The goal: select "Zinkos" in macOS Sound output and hear audio through a DAC connected to a Raspberry Pi 5.

**Audio path:** System audio → Zinkos CoreAudio driver → UDP → Pi 5 → ALSA → DAC

## Target Hardware

- **Mac:** macOS (Apple Silicon), sender side
- **Pi:** Raspberry Pi 5 8GB, receiver side, connected to USB/I2S DAC
- **Network:** Same Wi-Fi network (simultaneous internet use supported)

## Audio Format (Fixed)

- Sample rate: **44,100 Hz**
- Channels: **2** (stereo, interleaved)
- Format: **S16_LE** (16-bit signed little-endian PCM)
- Bytes per frame: 4 (2 channels × 2 bytes)
- Packet duration: **5 ms**
- Frames per packet: 221 (5ms × 44100 / 1000 ≈ 220.5, rounded up)
- Payload per packet: **884 bytes** (221 frames × 4 bytes)

## Latency Target

- End-to-end: **18–25 ms** stable on good Wi-Fi
- Jitter buffer on Pi: 10–15 ms
- Packet pacing: every 5 ms

## Architecture (Two Layers)

### Layer 1 — Driver Glue (thin C/C++ shim)
The AudioServerPlugIn entry points. Hard to unit test without macOS runtime. Keep as small as possible.
- Implements CoreAudio plugin interface (COM-like)
- Exposes device: name "Zinkos", manufacturer, UID
- Exposes stream: 1 output stream, 2ch, 44.1kHz, S16LE
- I/O start/stop lifecycle
- Calls into Layer 2 (engine) via C FFI

### Layer 2 — Engine (Rust, highly testable)
All real logic lives here.
- **RingBuffer** — lock-free SPSC between CoreAudio RT thread and network thread
- **Packetizer** — frames → UDP packets with header
- **UDPSender** — socket management, SO_SNDBUF, IP_TOS/DSCP, pacing
- **DriftEstimator** — monitors ring fill level, adjusts for clock drift (ppm-level)
- **StateMachine** — Stopped → Starting → Running → Stopping
- **Config** — target latency, packet duration, Pi address/port

### Pi Receiver (Rust)
- UDP receive loop
- Jitter buffer (configurable start-fill: 12–18 ms)
- ALSA output via `alsa` crate or direct FFI
- Target device: configurable (e.g., `hw:1,0`)

## UDP Packet Format

```
struct ZinkosPacket {
    seq: u32,          // sequence number
    frames: u32,       // e.g., 221
    timestamp_ns: u64, // optional, for jitter measurement
    pcm: [i16; frames * 2], // interleaved L/R samples
}
```

Total packet size: 16-byte header + 884-byte payload = 900 bytes (well under MTU)

## Language Choices

- **Engine + Pi receiver:** Rust
- **Driver shim:** C/C++ (minimal, ~hundreds of lines)
- **FFI boundary:** Rust `cdylib` called from C++ shim via `extern "C"` functions
- **Build:** CMake for driver bundle, Cargo for Rust crates
- **Tests:** `cargo test` for engine, CTest or shell scripts for integration

## Repo Layout

```
zinkos-driver/
  CLAUDE.md
  Cargo.toml                 # Workspace root
  CMakeLists.txt             # Top-level build (driver bundle + Rust)
  driver/                    # AudioServerPlugIn glue (C/C++)
    ZinkosPlugin.cpp         # Plugin entry point, QueryInterface, etc.
    ZinkosDevice.cpp         # Device/stream property handling
    ZinkosPlugin.h
    ZinkosDevice.h
    Info.plist               # Bundle metadata for CoreAudio
    CMakeLists.txt
  engine/                    # Rust crate — testable core (cdylib)
    Cargo.toml
    include/
      zinkos_engine.h        # C FFI header
    src/
      lib.rs                 # FFI exports (extern "C")
      ffi.rs                 # FFI boundary definitions
      ring_buffer.rs
      packetizer.rs
      udp_sender.rs
      pacer.rs
      drift.rs
      state_machine.rs
      config.rs
      bin/
        sender_cli.rs        # Feed engine without driver for testing
  receiver/                  # All receiver implementations
    Cargo.toml               # Rust receiver crate
    src/
      main.rs
      jitter_buffer.rs
      alsa_sink.rs
    c/                       # C receiver (production, runs on Thinkpad)
      zinkos_rx.c            # Single-file receiver (~200 lines)
      Makefile               # cc -O2 -o zinkos_rx zinkos_rx.c -lasound -lpthread
      zinkos-rx.service      # systemd unit file
  scripts/                   # CLI & build tools
    zinkos                   # CLI tool (bash)
    reload.sh                # Build, validate, install, reload
    install.sh               # Install driver bundle
    uninstall.sh             # Uninstall driver bundle
    validate.sh              # Pre-install validation
  tests/                     # Integration tests
    driver_load_test.c       # dlopen/dlsym test
  docs/
    system-overview.md       # Full system architecture doc
    latency-analysis.md      # Latency breakdown & tuning
```

## Latency

When making changes that affect latency (buffer sizes, pacing, ALSA config, jitter buffer tuning, etc.), update `docs/latency-analysis.md` with the new values and expected impact.

## Key Design Rules

1. **CoreAudio RT thread must never block.** Write to ring buffer only; no allocations, no locks, no syscalls.
2. **Network thread does the heavy lifting.** Reads ring buffer, packetizes, sends UDP. Can block/retry.
3. **Engine is 100% testable without CoreAudio.** All logic goes through the Rust engine crate.
4. **Driver shim is dumb glue.** It only translates CoreAudio callbacks into engine FFI calls.
5. **Fixed format simplifies everything.** No format negotiation — 44.1kHz stereo S16LE everywhere.

## What Can Be Tested Pre-Compilation

### Unit tests (pure logic, `cargo test`)
- Ring buffer: wrap-around, overrun/underrun, fill-level reporting
- Packetizer: correct bytes/packet for 44.1kHz, frame boundaries, sequence numbers
- Jitter/latency policy: target buffer depth produces expected start-fill
- State machine: idempotent start, clean shutdown from any state
- Threading: producer/consumer stress tests on ring buffer
- Network: MTU sanity (packets < 1400 bytes), pacing intervals

### Driver property tests (compile-time)
- Device name == "Zinkos"
- Supported sample rates include 44100
- Channel count == 2
- Reported latency values are sane

### Mock CoreAudio pull
- Simulate CoreAudio asking for N frames repeatedly
- Verify engine produces correct UDP packets

### Integration tests (macOS, no driver loading)
- CLI tool generates tone → feeds engine → verifies UDP packets on localhost
- Pi receiver can be tested with recorded packet streams

## What Requires Loading the Driver
- macOS enumerates it as output device in Sound menu
- Real IOProc callback timing/cadence
- Wake/sleep, device hog mode interactions
- System volume/mute integration
- Real end-to-end latency measurement

## Build & Install

Driver bundle installs to: `/Library/Audio/Plug-Ins/HAL/Zinkos.driver`

## CLI Tool (`zinkos`)

Located at `scripts/zinkos`. Install system-wide with `zinkos install` (symlinks to `/usr/local/bin/zinkos`).

```
zinkos status              # show current config + driver status
zinkos set ip <IP>         # set receiver IP
zinkos set port <PORT>     # set receiver port
zinkos set latency <MS>    # set latency offset
zinkos rebuild             # build, validate, install, and reload driver
zinkos reload              # restart coreaudiod to pick up config changes
zinkos install             # symlink to /usr/local/bin for global access
zinkos uninstall           # remove the global symlink
```

Config is stored in `com.zinkos.driver` plist (via `defaults write`). Volume/mute state is persisted to `/Library/Preferences/Audio/com.zinkos.volume` (plain file, written by the driver as `_coreaudiod`).

## Commands

- `cargo test` — run all engine + receiver unit tests
- `cargo build --release` — build engine cdylib and receiver binary
- `cmake --build build/` — build driver bundle (links against engine cdylib)
- `zinkos rebuild` — full build + validate + install + reload cycle
