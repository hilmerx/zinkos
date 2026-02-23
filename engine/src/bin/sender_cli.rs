//! Sender CLI — test the full engine pipeline without CoreAudio.
//!
//! Generates a 440Hz sine wave and feeds it through the engine's
//! ring buffer → packetizer → UDP sender chain.
//! Identical to the driver path: same pacing, QoS, drift monitoring, packet format.
//!
//! Usage:
//!   cargo run --bin sender-cli -- --ip 192.168.1.100
//!   cargo run --bin sender-cli -- --ip 192.168.1.100 --port 4010 --duration 10 --freq 880

use std::time::{Duration, Instant};

use zinkos_engine::config::{
    EngineConfig, CHANNELS, DEFAULT_FRAMES_PER_PACKET, SAMPLE_RATE,
};
use zinkos_engine::ffi::ZinkosEngine;
use zinkos_engine::pacer::Pacer;

fn main() {
    let ip = std::env::args()
        .position(|a| a == "--ip")
        .and_then(|i| std::env::args().nth(i + 1))
        .unwrap_or_else(|| "127.0.0.1".to_string());

    let port: u16 = std::env::args()
        .position(|a| a == "--port")
        .and_then(|i| std::env::args().nth(i + 1))
        .and_then(|s| s.parse().ok())
        .unwrap_or(4010);

    let duration_secs: u64 = std::env::args()
        .position(|a| a == "--duration")
        .and_then(|i| std::env::args().nth(i + 1))
        .and_then(|s| s.parse().ok())
        .unwrap_or(5);

    let freq: f64 = std::env::args()
        .position(|a| a == "--freq")
        .and_then(|i| std::env::args().nth(i + 1))
        .and_then(|s| s.parse().ok())
        .unwrap_or(440.0);

    println!("Zinkos sender CLI");
    println!("  Target: {ip}:{port}");
    println!("  Duration: {duration_secs}s");
    println!("  Tone: {freq}Hz sine wave");
    println!();

    let config = EngineConfig {
        target_ip: ip,
        target_port: port,
        ..EngineConfig::default()
    };

    let mut engine = ZinkosEngine::new(config);

    engine.start().expect("failed to start engine");
    println!("Engine started, streaming...");

    let frames_per_chunk = DEFAULT_FRAMES_PER_PACKET as usize;
    let samples_per_chunk = frames_per_chunk * CHANNELS as usize;
    let chunk_duration = Duration::from_nanos(
        (frames_per_chunk as u64 * 1_000_000_000) / SAMPLE_RATE as u64,
    );

    let mut pacer = Pacer::new(chunk_duration);
    let mut sample_index: u64 = 0;
    let start = Instant::now();
    let mut total_frames: u64 = 0;

    while start.elapsed() < Duration::from_secs(duration_secs) {
        // Generate sine wave chunk
        let mut buf = vec![0i16; samples_per_chunk];
        for i in 0..frames_per_chunk {
            let t = (sample_index + i as u64) as f64 / SAMPLE_RATE as f64;
            let sample = (t * freq * 2.0 * std::f64::consts::PI).sin();
            let val = (sample * 12000.0) as i16;
            buf[i * 2] = val;     // L
            buf[i * 2 + 1] = val; // R
        }

        let written = engine.write_frames(&buf);
        total_frames += written as u64;
        sample_index += frames_per_chunk as u64;

        pacer.wait();
    }

    println!();
    println!("Sent {total_frames} frames ({:.1}s of audio)", total_frames as f64 / SAMPLE_RATE as f64);

    engine.stop().expect("failed to stop engine");
    println!("Engine stopped.");
}
