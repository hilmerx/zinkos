# Zinkos Latency Analysis

Last updated: 2026-02-23

## Audio Path

```
CoreAudio IO buffer → DoIOOperation (Float32→S16LE) → Ring buffer
  → Pacer → Packetizer → UDP send
  → Wi-Fi
  → Receiver UDP recv → Jitter buffer → ALSA buffer → DAC
```

## Stage-by-Stage Breakdown

### Sender Side (Mac)

| Stage | Latency | Fixed? | Notes |
|-------|---------|--------|-------|
| CoreAudio IO buffer (128 frames @ 48kHz) | ~2.67ms | Yes (OS-controlled) | Data is ~2.67ms old when DoIOOperation fires. Buffer size reported via `'fsiz'` property. |
| DoIOOperation (Float32→S16LE + volume) | <0.01ms | Yes | In-place conversion, negligible |
| Ring buffer write | <0.01ms | Yes | Lock-free SPSC (`rtrb`), RT-safe |
| Wait for next pacer tick | 0–N ms (avg ~N/2) | Tunable | Pacer fires every `frames_per_packet / 48000` seconds. Default 240 frames = 5ms, tuned 100 frames = ~2ms. |
| Drain IO buffer at configured period | ~2–5ms | Linked to IO buffer + period | 128 frames drains in 1 tick at 240-frame period, or ~2 ticks at 100-frame period |
| UDP send (syscall) | <0.1ms | Yes | Single `send_to`, well under MTU |

**Sender total (default): ~5–8ms** (IO buffer + pacer phase)
**Sender total (tuned): ~4–5ms** (128-frame IO buffer + 100-frame period)

### Network

| Stage | Latency | Notes |
|-------|---------|-------|
| Wi-Fi UDP one-way (same LAN) | ~1–3ms typical | Can spike 10–30ms with contention/retransmission |
| DSCP EF marking | 0ms | `IP_TOS = 46 << 2`, helps on managed networks, ignored on most home routers |

**Network total: ~2ms typical, ~10–30ms worst case**

### Receiver Side (Linux)

| Stage | Latency | Fixed? | Notes |
|-------|---------|--------|-------|
| UDP recv → jitter buffer push | <0.1ms | Yes | Separate recv_thread with RT priority |
| **Jitter buffer start-fill** | **configurable** | **Tunable** | Set via `install.sh` (argv[2]). Must absorb Wi-Fi jitter. See tuning section. |
| **ALSA buffer** | **configurable** | **Tunable** | `period_size × 3` periods. Set via `install.sh` (argv[3]). See tuning section. |
| ALSA start_threshold | configurable | Tunable | Controls when hardware begins DMA. `= period_size` is lowest safe value. |
| DAC | ~1ms | Yes (hardware) | USB DAC adds ~1ms, I2S negligible |

**Receiver total: depends entirely on configuration (see below)**

## End-to-End Latency Formula

### Default settings (240-frame period, 15ms start-fill)

```
total = IO_buffer + pacer_phase + network + jitter_fill + DAC
      ≈ 2.67      + 2.5         + 2       + 15          + 1
      ≈ ~23ms
```

### Tuned settings (100-frame period, 3ms start-fill)

```
total = IO_buffer + pacer_phase + network + jitter_fill + DAC
      ≈ 2.67      + 1.0         + 2       + 3           + 1
      ≈ ~9ms
```

## Receiver Tuning Guide

### Jitter Buffer Start-Fill

How much audio to accumulate before starting playback. Must absorb Wi-Fi jitter. Set during `install.sh` or passed as argv[2] to the receiver binary.

| Value | Behavior |
|-------|----------|
| 3ms | Aggressive. Works on clean Wi-Fi with no other traffic. Achieves ~9ms total. |
| 10ms | Moderate. Works on most home networks. |
| 12–15ms | **Default.** Absorbs typical Wi-Fi jitter (1–10ms). Rock-solid. |
| 25ms | Conservative. Handles occasional Wi-Fi spikes. |

### ALSA Period Size

The ALSA period controls how many frames are written to hardware at a time. Set during `install.sh` or passed as argv[3] to the receiver binary. Should generally match the sender's `FramesPerPacket` for best results.

| Period | Duration at 48kHz | Buffer (×3) | Behavior |
|--------|-------------------|-------------|----------|
| 100 | ~2ms | 300 frames = 6.25ms | **Tuned.** Matches 100-frame sender period. |
| 240 | 5ms | 720 frames = 15ms | **Default.** Matches default sender period. |
| 480 | 10ms | 1440 frames = 30ms | Conservative. More scheduling headroom. |

### ALSA `start_threshold`

When ALSA begins DMA playback from its buffer.

| Value | Latency added | Behavior |
|-------|---------------|----------|
| `period_size` (1 period) | 1 period | **Recommended.** Start playing after first period is written. |
| `period_size × 2` | 2 periods | Safer — 1 period of cushion before underrun. |
| `buffer_size` (all periods) | Full buffer latency | Bad for latency — waits until entire buffer is full. |

### Configuration Profiles

**Default (~25ms total):** Rock-solid on any network.

```
Sender:
  FramesPerPacket   = 240       // 5ms @ 48kHz (set in Swift app or plist)
  IO buffer         = 128       // 2.67ms (fixed in driver)

Receiver:
  start_fill_ms     = 15        // 720 frames at 48kHz
  ALSA period_size  = 240       // 5ms (matches sender)
  ALSA periods      = 3         // 15ms total buffer
  ALSA start_thresh = period*2  // 480 frames = 10ms
```

**Tuned (~9ms total):** Requires good Wi-Fi with low jitter.

```
Sender:
  FramesPerPacket   = 100       // ~2ms @ 48kHz (set in Swift app)
  IO buffer         = 128       // 2.67ms (fixed in driver)

Receiver:
  start_fill_ms     = 3         // 144 frames at 48kHz
  ALSA period_size  = 100       // ~2ms (matches sender)
  ALSA periods      = 3         // 300 frames = 6.25ms total buffer
  ALSA start_thresh = period*2  // 200 frames = ~4ms
```

| Stage | Default | Tuned |
|-------|---------|-------|
| CoreAudio IO buffer | 2.67ms (128 frames) | 2.67ms (128 frames) |
| Sender pacing | ~5ms | ~2ms |
| Network (Wi-Fi UDP) | ~1–2ms | ~1–2ms |
| Receiver start-fill | 15ms | 3ms |
| DAC | ~1ms | ~1ms |
| **Total** | **~25ms** | **~9ms** |

## Constants Reference

### Sender (engine/src/config.rs + driver/ZinkosDevice.h)

| Constant | Value | Location |
|----------|-------|----------|
| `SAMPLE_RATE` | 48,000 Hz | config.rs, ZinkosDevice.h |
| `CHANNELS` | 2 (stereo) | config.rs |
| `DEFAULT_FRAMES_PER_PACKET` | 240 (5ms) | config.rs |
| `HEADER_BYTES` | 20 | config.rs |
| `PROTO_MAGIC` | 0x5A4B ("ZK") | config.rs |
| `PROTO_VERSION` | 1 | config.rs |
| `RING_BUFFER_FRAMES` | 4,800 (~100ms) | config.rs |
| IO buffer size | 128 frames (2.67ms) | ZinkosDevice.h (`'fsiz'`) |
| ZeroTimeStampPeriod | 128 | ZinkosDevice.cpp |
| DeviceLatency | 128 frames (2.67ms) | ZinkosDevice.h |
| StreamLatency | 128 frames (2.67ms) | ZinkosDevice.h |
| SafetyOffset | 64 frames (1.33ms) | ZinkosDevice.h |

### Configurable Values (plist: com.zinkos.driver)

| Key | Default | Range | Set via |
|-----|---------|-------|---------|
| `ReceiverIP` | (none) | IPv4 or hostname | Swift app or `zinkos set ip` |
| `ReceiverPort` | 4010 | 1–65535 | Swift app or `zinkos set port` |
| `FramesPerPacket` | 240 | 48–4800 | Swift app Period field |
| `LatencyOffsetMs` | 0 | 0+ | `defaults write` or Swift app |

### Reported Latency to CoreAudio

CoreAudio uses `DeviceLatency + StreamLatency + SafetyOffset` for AV sync:
```
128 + 128 + 64 = 320 frames = 6.67ms
```
This should approximate actual end-to-end latency for correct AV sync. The `LatencyOffsetMs` plist value adds additional offset if needed.

## Driver-Side Latency Offset (`LatencyOffsetMs`)

The driver reads an optional `LatencyOffsetMs` value from `com.zinkos.driver` plist (set via `defaults write /Library/Preferences/com.zinkos.driver LatencyOffsetMs <value>` or the Swift setup app). This adds a fixed offset to the latency reported to CoreAudio, which affects AV sync compensation. Does not affect actual audio path latency — only the value reported for sync purposes.

## Potential Future Optimizations

### Sender side (diminishing returns)
- **Aggressive ring drain:** Send immediately when frames are available instead of waiting for next pacer tick. Saves ~1ms avg but risks bursty packet arrival at receiver.
- **Adaptive pacing:** Monitor ring fill and send faster when behind. Complex, marginal benefit.

### Receiver side (where the big wins are)
- **Adaptive jitter buffer:** Start with configured fill, dynamically grow/shrink based on observed jitter. Best of both worlds.
- **ALSA direct hardware (mmap):** Skip kernel buffering, write directly to DMA buffer. Complex but could save a few ms.
- **Receiver-side drift compensation:** If Mac and receiver clocks drift, insert/drop a sample every N frames to stay in sync. Prevents long-term underruns without needing large buffers.
