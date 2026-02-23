mod alsa_sink;
mod jitter_buffer;

use std::net::UdpSocket;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use std::thread;
use std::time::Duration;

use clap::Parser;
use zinkos_engine::config::{DEFAULT_PORT, DEFAULT_FRAMES_PER_PACKET};

use alsa_sink::AlsaSink;
use jitter_buffer::{JitterBuffer, JitterState};

#[derive(Parser)]
#[command(name = "zinkos-receiver", about = "Zinkos Wi-Fi audio receiver")]
struct Args {
    /// UDP port to listen on
    #[arg(long, default_value_t = DEFAULT_PORT)]
    port: u16,

    /// ALSA device name (Linux only)
    #[arg(long, default_value = "hw:1,0")]
    device: String,

    /// Jitter buffer start-fill in milliseconds
    #[arg(long, default_value_t = 12)]
    jitter_ms: u32,
}

fn main() {
    let args = Args::parse();

    println!(
        "Zinkos receiver — port:{} device:{} jitter:{}ms",
        args.port, args.device, args.jitter_ms
    );

    let jitter_buf = Arc::new(Mutex::new(JitterBuffer::new(args.jitter_ms)));
    let running = Arc::new(AtomicBool::new(true));

    // Handle Ctrl+C
    let running_sig = Arc::clone(&running);
    ctrlc_handler(running_sig);

    // Receive thread
    let jb_recv = Arc::clone(&jitter_buf);
    let running_recv = Arc::clone(&running);
    let recv_thread = thread::spawn(move || {
        receive_loop(args.port, jb_recv, running_recv);
    });

    // Playback thread
    let jb_play = Arc::clone(&jitter_buf);
    let running_play = Arc::clone(&running);
    let play_thread = thread::spawn(move || {
        playback_loop(&args.device, jb_play, running_play);
    });

    recv_thread.join().unwrap();
    play_thread.join().unwrap();

    // Print stats
    let jb = jitter_buf.lock().unwrap();
    println!("\n--- Stats ---");
    println!("Packets received: {}", jb.packets_received);
    println!("Packets dropped:  {}", jb.packets_dropped);
    println!("Silence inserted: {} frames", jb.silence_frames_inserted);
    println!("Underruns:        {}", jb.underruns);
}

fn receive_loop(
    port: u16,
    jitter_buf: Arc<Mutex<JitterBuffer>>,
    running: Arc<AtomicBool>,
) {
    let addr = format!("0.0.0.0:{port}");
    let socket = UdpSocket::bind(&addr).expect("failed to bind UDP socket");
    socket
        .set_read_timeout(Some(Duration::from_millis(100)))
        .unwrap();

    println!("Listening on {addr}");

    let mut buf = [0u8; 2048]; // oversized to handle any frames_per_packet

    while running.load(Ordering::Relaxed) {
        match socket.recv_from(&mut buf) {
            Ok((n, _from)) => {
                let mut jb = jitter_buf.lock().unwrap();
                jb.push(&buf[..n]);
            }
            Err(ref e) if e.kind() == std::io::ErrorKind::WouldBlock => continue,
            Err(ref e) if e.kind() == std::io::ErrorKind::TimedOut => continue,
            Err(e) => {
                eprintln!("recv error: {e}");
                break;
            }
        }
    }
}

fn playback_loop(
    device: &str,
    jitter_buf: Arc<Mutex<JitterBuffer>>,
    running: Arc<AtomicBool>,
) {
    let mut sink = AlsaSink::open(device).expect("failed to open audio output");

    let frames_per_write = DEFAULT_FRAMES_PER_PACKET as usize;
    let sleep_duration = Duration::from_millis(4); // slightly less than 5ms to stay ahead

    while running.load(Ordering::Relaxed) {
        let frames = {
            let mut jb = jitter_buf.lock().unwrap();
            if jb.state() == JitterState::Playing {
                jb.pop_frames(frames_per_write)
            } else {
                vec![]
            }
        };

        if frames.is_empty() {
            thread::sleep(sleep_duration);
            continue;
        }

        if let Err(e) = sink.write_frames(&frames) {
            eprintln!("audio write error: {e}");
        }

        thread::sleep(sleep_duration);
    }
}

fn ctrlc_handler(running: Arc<AtomicBool>) {
    // Simple approach: just set running to false on signal
    // (we don't add a signal crate dependency for this)
    thread::spawn(move || {
        // Read stdin for 'q' as a quit mechanism on platforms without signal handling
        let mut input = String::new();
        loop {
            input.clear();
            if std::io::stdin().read_line(&mut input).is_ok() {
                if input.trim() == "q" {
                    running.store(false, Ordering::Relaxed);
                    break;
                }
            }
        }
    });
}
