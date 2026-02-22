# Zinkos Latency Analysis

Last updated: 2026-02-22

## Audio Path

```
CoreAudio IO buffer → DoIOOperation (Float32→S16LE) → Ring buffer
  → Pacer (5ms) → Packetizer → UDP send
  → Wi-Fi
  → Receiver UDP recv → Jitter buffer → ALSA buffer → DAC
```

## Stage-by-Stage Breakdown

### Sender Side (Mac)

| Stage | Latency | Fixed? | Notes |
|-------|---------|--------|-------|
| CoreAudio IO buffer (512 frames @ 48kHz) | ~10.7ms | Yes (OS-controlled) | Data is ~10.7ms old when DoIOOperation fires. Buffer size reported via `'fsiz'` property. |
| DoIOOperation (Float32→S16LE + volume) | <0.01ms | Yes | In-place conversion, negligible |
| Ring buffer write | <0.01ms | Yes | Lock-free SPSC (`rtrb`), RT-safe |
| Wait for next pacer tick | 0–5ms (avg ~2.5ms) | Tunable | Pacer fires every 5ms (`PACKET_DURATION_MS`). Data waits for next tick. |
| Drain 512 frames at 240/packet | ~10ms | Linked to IO buffer | Takes 2–3 pacer ticks to fully drain one CoreAudio batch |
| UDP send (syscall) | <0.1ms | Yes | Single `send_to`, ~976 bytes, well under MTU |

**Sender total: ~13–18ms** (dominated by CoreAudio IO buffer + pacer phase)

### Network

| Stage | Latency | Notes |
|-------|---------|-------|
| Wi-Fi UDP one-way (same LAN) | ~1–3ms typical | Can spike 10–30ms with contention/retransmission |
| DSCP EF marking | 0ms | `IP_TOS = 46 << 2`, helps on managed networks, ignored on most home routers |

**Network total: ~2ms typical, ~10–30ms worst case**

### Receiver Side (Pi / Laptop)

| Stage | Latency | Fixed? | Notes |
|-------|---------|--------|-------|
| UDP recv → jitter buffer push | <0.1ms | Yes | Separate recv_thread with RT priority |
| **Jitter buffer start-fill** | **configurable** | **Tunable** | `START_FILL_MS` — must absorb Wi-Fi jitter. See tuning section. |
| **ALSA buffer** | **configurable** | **Tunable** | `period_size × N` periods. See tuning section. |
| ALSA start_threshold | configurable | Tunable | Controls when hardware begins DMA. `= period_size` is lowest safe value. |
| DAC | ~1ms | Yes (hardware) | USB DAC adds ~1ms, I2S negligible |

**Receiver total: depends entirely on configuration (see below)**

## End-to-End Latency Formula

```
total = IO_buffer + pacer_phase + network + jitter_fill + ALSA_buffer + DAC
      ≈ 10.7     + 2.5         + 2       + jitter_fill + ALSA_latency + 1
      ≈ 16.2ms + jitter_fill + ALSA_latency
```

## Receiver Tuning Guide

### Jitter Buffer Start-Fill (`START_FILL_MS`)

How much audio to accumulate before starting playback. Must absorb Wi-Fi jitter.

| Value | Behavior |
|-------|----------|
| 10ms | Aggressive. Works on clean Wi-Fi with no other traffic. Fragile. |
| 12–15ms | **Recommended.** Absorbs typical Wi-Fi jitter (1–10ms). |
| 25ms | Conservative. Handles occasional Wi-Fi spikes. |
| 60ms | Overkill. Only needed if ALSA pre-fill is draining the ring buffer (fix the root cause instead). |

### ALSA Buffer Size (`period_size × N`)

Total ALSA hardware buffer. Larger = more resilient to scheduling delays, but adds latency.

| Periods | Buffer at 240-frame period (48kHz) | Behavior |
|---------|------------------------------------|----------|
| × 3 | 720 frames = 15ms | **Recommended.** Low latency, needs RT priority on playback thread. |
| × 4 | 960 frames = 20ms | Safe default. Tolerates minor scheduling hiccups. |
| × 8 | 1920 frames = 40ms | Overkill. Was used to fix a pre-fill issue that should be solved differently. |

### ALSA `start_threshold`

When ALSA begins DMA playback from its buffer.

| Value | Latency added | Behavior |
|-------|---------------|----------|
| `period_size` (1 period = 5ms) | ~5ms | **Recommended.** Start playing after first period is written. |
| `period_size × 2` | ~10ms | Safer — 1 period of cushion before underrun. |
| `buffer_size` (all periods) | Full buffer latency | Bad for latency — waits until entire buffer is full. |

### Recommended Receiver Configuration (Low Latency)

```
START_FILL_MS     = 15      // 720 frames at 48kHz
ALSA period_size  = 240     // 5ms (= FRAMES_PER_PACKET)
ALSA periods      = 3       // 15ms total buffer
ALSA start_thresh = 240     // start after 1 period
Pre-fill           = 2 periods (480 frames = 10ms)
```

**Expected receiver latency: ~15ms**
**Expected total end-to-end: ~18–22ms** (matching target in CLAUDE.md)

### Current Configuration (Rust receiver, as of 2026-02-22)

```
Jitter buffer     = 12ms (--jitter_ms default)
ALSA period_size  = 240 (5ms)
ALSA periods      = 3 (15ms total buffer)
ALSA start_thresh = 480 (2 periods = 10ms)
ALSA avail_min    = 240 (1 period)
```

**Expected end-to-end: ~25–30ms**

### Current C Receiver Configuration (hilmerx:/home/hilmerx/zinkos_rx_44100.c)

```
START_FILL_MS     = 15      // 720 frames at 48kHz
ALSA period_size  = 240 (5ms)
ALSA periods      = 3       // 15ms total buffer
ALSA start_thresh = period * 2 (480 = 10ms)
ALSA avail_min    = period (240)
Pre-fill          = up to buffer_size, with ring buffer cushion guard
```

**Expected end-to-end: ~25–30ms**

### Previous C Receiver Configuration (before 2026-02-22)

```
START_FILL_MS     = 60      // 2880 frames — WAY too high
ALSA period_size  = 240
ALSA periods      = 8       // 40ms buffer — too large
ALSA start_thresh = buffer_size (1920) — adds full 40ms
Pre-fill          = fills entire ALSA buffer
```

**Measured end-to-end: ~100–120ms**

## Constants Reference

### Sender (engine/src/config.rs + driver/ZinkosDevice.h)

| Constant | Value | Location |
|----------|-------|----------|
| `SAMPLE_RATE` | 48,000 Hz | config.rs, ZinkosDevice.h |
| `CHANNELS` | 2 (stereo) | config.rs |
| `FRAMES_PER_PACKET` | 240 (5ms) | config.rs |
| `PACKET_BYTES` | 976 (16 hdr + 960 pcm) | config.rs |
| `RING_BUFFER_FRAMES` | 4,800 (~100ms) | config.rs |
| IO buffer size | 512 frames (10.67ms) | ZinkosDevice.h (`'fsiz'`) |
| ZeroTimeStampPeriod | 512 | ZinkosDevice.cpp |
| DeviceLatency | 480 frames (10ms) | ZinkosDevice.h |
| StreamLatency | 240 frames (5ms) | ZinkosDevice.h |
| SafetyOffset | 240 frames (5ms) | ZinkosDevice.h |

### Reported Latency to CoreAudio

CoreAudio uses `DeviceLatency + StreamLatency + SafetyOffset` for AV sync:
```
480 + 240 + 240 = 960 frames = 20ms
```
This should match actual end-to-end latency for correct AV sync. Update these values if receiver tuning changes total latency significantly.

## Driver-Side Latency Offset (`LatencyOffsetMs`)

The driver reads an optional `LatencyOffsetMs` value from `com.zinkos.driver` plist (set via `defaults write com.zinkos.driver LatencyOffsetMs <value>`). This adds a fixed offset to the latency reported to CoreAudio, which affects AV sync compensation. Use this if your actual end-to-end latency differs from what CoreAudio assumes (960 frames = 20ms). Does not affect actual audio path latency — only the value reported for sync purposes.

## Potential Future Optimizations

### Sender side (diminishing returns)
- **Aggressive ring drain:** Send immediately when 240 frames are available instead of waiting for next 5ms tick. Saves ~2ms avg but risks bursty packet arrival at receiver.
- **Smaller IO buffer:** Request 256 frames instead of 512. CoreAudio may or may not honor it. Would halve the IO buffer latency to ~5.3ms.
- **Adaptive pacing:** Monitor ring fill and send faster when behind. Complex, marginal benefit.

### Receiver side (where the big wins are)
- **Adaptive jitter buffer:** Start with 15ms fill, dynamically grow/shrink based on observed jitter. Best of both worlds.
- **ALSA direct hardware (mmap):** Skip kernel buffering, write directly to DMA buffer. Saves ~5ms but complex.
- **Receiver-side drift compensation:** If Mac and Pi clocks drift, insert/drop a sample every N frames to stay in sync. Prevents long-term underruns without needing large buffers.
