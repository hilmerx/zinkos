<p align="center">
  <img src="logo.png" alt="Zinkos" width="200">
</p>

<h3 align="center">Ultra low-latency Wi-Fi audio streaming</h3>

<p align="center">
  macOS CoreAudio driver + Linux receiver<br>
  ~25ms end-to-end (and improving) &bull; Lossless PCM &bull; UDP
</p>

---

Select **Zinkos** as your sound output on macOS. Audio streams over your local network to a Linux receiver — a Raspberry Pi, an old laptop, any machine running ALSA. Connect a DAC or plug in speakers and you have a wireless audio endpoint with near-zero latency.

Watch films, YouTube, game, or video call — all in sync with your wirelessly connected speakers, no lip-sync issues. No proprietary hardware, no Apple ecosystem lock-in, no subscriptions. Just Wi-Fi and open source software.


<p align="center">
  <img src="menu.png" alt="Zinkos in macOS Sound output menu" width="420">
</p>


Zinkos appears as a native sound output in macOS — no menu bar app, no background process to manage. Just select it like you would AirPods or built-in speakers. All system audio is captured and streamed instantly.

- **Free and open source** — MIT licensed, no accounts, no subscriptions, no cloud
- **No special hardware** — any Mac as sender, any Linux box as receiver (Raspberry Pi, old laptop, anything with ALSA)
- **Lossless** — uncompressed PCM, bit-perfect audio with no codec artifacts
- **Low latency** — fast enough for video, casual gaming, and everyday use
- **Lightweight** — the receiver is a single C binary under 300 lines, runs on a Pi Zero
- **Simple** — build, set the receiver IP, pick Zinkos in Sound settings, done

## How It Works

```
     MacBook              Wi-Fi           Linux (receiver)
┌────────────────┐     ┌──────────┐      ┌──────────────────┐
│ System audio   │     │  5ms UDP │      │ UDP recv thread  │
│      │         │     │  packets │      │      │           │
│      ▼         │     │  976B    │      │      ▼           │
│ CoreAudio ─────│─────│──────────│─────▶│ Ring buffer      │
│      │         │     │          │      │      │           │
│      ▼         │     └──────────┘      │      ▼           │
│ Zinkos driver  │                       │ Jitter buffer    │
│ (Rust engine)  │                       │      │           │
└────────────────┘                       │      ▼           │
                                         │ ALSA → DAC       │
                                         └──────────────────┘
```

## Latency Comparison

| | Latency | Quality | Hardware | Downsides |
|---|---|---|---|---|
| **Zinkos** | **~25ms (and improving)** | **Lossless PCM** | **Standard Wi-Fi** | **Requires manual setup** |
| AirPlay 2 | ~2000ms | Lossless | Apple ecosystem | Unusable latency for video, Apple-only, no Linux receivers |
| Bluetooth A2DP | ~150ms | Lossy | BT adapter on both ends | Compressed audio, short range, pairs with one device |
| Snapcast | ~30–50ms | Lossless | Standard Wi-Fi | No native macOS output — requires piping audio manually |
| Dante (pro AV) | ~2ms | Lossless | Dedicated hardware both ends | Expensive licensing, proprietary hardware, not consumer-grade |

Zinkos sits between consumer wireless and pro AV gear — no special hardware or licensing needed. Current ~25ms latency will improve further with planned enhancements like adaptive jitter buffering and clock drift compensation.

## Installation

### Prerequisites

**Mac (sender):**
- macOS (Apple Silicon or Intel)
- [Rust](https://rustup.rs/) — `curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh`
- [CMake](https://cmake.org/) — `brew install cmake`
- Apple Developer certificate (free account works) — macOS requires signed audio plugins. Open Xcode → Settings → Accounts → add your Apple ID → Manage Certificates → create an Apple Development certificate. The build auto-detects it.

**Linux (receiver):**
- Any Linux machine (Raspberry Pi, old laptop, server — anything with ALSA)
- C compiler — `sudo pacman -S base-devel` (Arch) or `sudo apt install build-essential` (Debian/Ubuntu)
- ALSA dev headers — `sudo pacman -S alsa-lib` (Arch) or `sudo apt install libasound2-dev` (Debian/Ubuntu)

### Mac (sender)

```bash
git clone https://github.com/hilmerx/zinkos.git
cd zinkos

# First-time build setup
mkdir build && cd build && cmake .. && cd ..

# Build, validate, install, and reload the driver
./scripts/zinkos rebuild

# Install the CLI tool system-wide
./scripts/zinkos install

# Point to your receiver's IP
zinkos set ip 192.168.1.87
zinkos reload

# Select "Zinkos" in System Settings → Sound → Output
```

### Linux (receiver)

```bash
git clone https://github.com/hilmerx/zinkos.git
cd zinkos/receiver/c

# Interactive installer — builds, lists audio devices, installs service
./install.sh
```

The installer lists your ALSA devices and lets you pick one:

```
Available audio devices:

  1) hw:0,0  —  bcm2835 Headphones [bcm2835 Headphones]
  2) hw:1,0  —  USB Audio [USB Audio]

Select device [1-2]:
```

## CLI Tool

<p align="center">
  <img src="CLI.png" alt="Zinkos CLI" width="480">
</p>

```
zinkos status       # show config + driver health
zinkos set ip <IP>  # set receiver IP
zinkos set port <N> # set receiver port
zinkos rebuild      # compile, validate, install, reload
zinkos reload       # restart coreaudiod
zinkos install      # symlink to /usr/local/bin
```

## Architecture

| Component | Language | Role |
|-----------|----------|------|
| Driver shim | C++ | CoreAudio plugin interface (Apple requires C/C++) |
| Engine | Rust | Ring buffer, packetizer, UDP sender, drift estimation |
| Receiver | C | UDP recv, jitter buffer, ALSA playback |
| CLI | Bash | Config management, build, reload |

The engine is 100% testable without CoreAudio:

```bash
cargo test
```

## Audio Format

Fixed across the entire pipeline — no negotiation, no codecs:

- **48,000 Hz** stereo
- **S16_LE** (16-bit signed little-endian PCM)
- **976-byte** UDP packets (16B header + 960B PCM)
- **UDP port 4010** default (configurable via `zinkos set port <N>`)

## Roadmap

- Configurable sample rate (44.1kHz / 96kHz)
- Adaptive jitter buffer (auto-tune to network conditions)
- Clock drift compensation (sample insertion/dropping)
- Bonjour/mDNS auto-discovery (no manual IP config)
- macOS Swift menu bar app
- Multi-room / multi-receiver support
- Forward error correction (FEC) for lossy networks
- Pre-built signed installer (no Xcode or developer certificate needed)

## Documentation

- **[System Overview](docs/overview.md)** — Full architecture, glossary, latency breakdown, competitor comparison
- **[Latency Analysis](docs/latency-analysis.md)** — Stage-by-stage latency breakdown and receiver tuning guide

## License

[MIT](LICENSE)
