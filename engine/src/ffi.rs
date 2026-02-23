use std::ffi::CStr;
use std::os::raw::c_char;
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};
use std::thread;
use std::time::{Duration, Instant};

use crate::config::EngineConfig;
use crate::drift::DriftEstimator;
use crate::pacer::Pacer;
use crate::packetizer::Packetizer;
use crate::ring_buffer::{RingBuffer, RingConsumer, RingProducer};
use crate::state_machine::{State, StateMachine};
use crate::udp_sender::UdpSender;

pub struct ZinkosEngine {
    config: EngineConfig,
    state: Arc<StateMachine>,
    producer: Option<RingProducer>,
    net_thread: Option<thread::JoinHandle<()>>,
    net_stop: Arc<AtomicBool>,
}

impl ZinkosEngine {
    pub fn new(config: EngineConfig) -> Self {
        Self {
            config,
            state: Arc::new(StateMachine::new()),
            producer: None,
            net_thread: None,
            net_stop: Arc::new(AtomicBool::new(false)),
        }
    }

    pub fn start(&mut self) -> Result<(), String> {
        self.state
            .start()
            .map_err(|s| format!("cannot start from state {s:?}"))?;

        let rb = RingBuffer::new(self.config.ring_buffer_frames);
        let (producer, consumer) = rb.split();
        self.producer = Some(producer);

        self.net_stop.store(false, Ordering::Release);

        let state = Arc::clone(&self.state);
        let stop = Arc::clone(&self.net_stop);
        let ip = self.config.target_ip.clone();
        let port = self.config.target_port;
        let frames_per_packet = self.config.frames_per_packet;
        let packet_duration_ms = self.config.packet_duration_ms();

        self.net_thread = Some(thread::spawn(move || {
            network_thread(consumer, state, stop, &ip, port, frames_per_packet, packet_duration_ms);
        }));

        Ok(())
    }

    pub fn stop(&mut self) -> Result<(), String> {
        self.state
            .stop()
            .map_err(|s| format!("cannot stop from state {s:?}"))?;

        self.net_stop.store(true, Ordering::Release);

        if let Some(handle) = self.net_thread.take() {
            handle.join().map_err(|_| "network thread panicked".to_string())?;
        }

        self.producer = None;

        self.state
            .mark_stopped()
            .map_err(|s| format!("failed to mark stopped: {s:?}"))?;

        Ok(())
    }

    /// Write interleaved i16 frames into the ring buffer.
    /// RT-safe: no allocation, no lock, no syscall.
    /// Returns the number of frames written.
    pub fn write_frames(&mut self, data: &[i16]) -> u32 {
        if !self.state.is_accepting_audio() {
            return 0;
        }

        match self.producer.as_mut() {
            Some(prod) => prod.write_frames(data) as u32,
            None => 0,
        }
    }

    pub fn get_state(&self) -> State {
        self.state.get()
    }
}

fn network_thread(
    mut consumer: RingConsumer,
    state: Arc<StateMachine>,
    stop: Arc<AtomicBool>,
    target_ip: &str,
    target_port: u16,
    frames_per_packet: u32,
    packet_duration_ms: u64,
) {
    let sender = match UdpSender::new(target_ip, target_port) {
        Ok(s) => s,
        Err(e) => {
            eprintln!("zinkos: failed to create UDP socket: {e}");
            let _ = state.mark_running(); // still transition so stop() works
            return;
        }
    };

    let _ = state.mark_running();

    let mut packetizer = Packetizer::new();
    let mut drift = DriftEstimator::new();
    let mut pacer = Pacer::new(Duration::from_millis(packet_duration_ms));
    let mut frame_buf = vec![[0i16; 2]; frames_per_packet as usize];
    let start_time = Instant::now();

    while !stop.load(Ordering::Acquire) {
        // Single clock: wait for next 5ms deadline, then read and send
        pacer.wait();

        let read = consumer.read_frames(&mut frame_buf);

        if read > 0 {
            let timestamp_ns = start_time.elapsed().as_nanos() as u64;
            let packet = packetizer.build_packet(&frame_buf[..read], timestamp_ns);

            if let Err(e) = sender.send(&packet) {
                eprintln!("zinkos: UDP send error: {e}");
            }

            drift.push(consumer.fill_level());
        }
    }
}

// === C FFI exports ===

/// Create a new engine. Returns null on failure.
/// `target_ip` must be a valid C string (null-terminated).
/// `frames_per_packet` = 0 means use the default (240).
#[no_mangle]
pub unsafe extern "C" fn zinkos_engine_create(
    target_ip: *const c_char,
    target_port: u16,
    frames_per_packet: u32,
) -> *mut ZinkosEngine {
    if target_ip.is_null() {
        return std::ptr::null_mut();
    }

    let ip = match CStr::from_ptr(target_ip).to_str() {
        Ok(s) => s.to_string(),
        Err(_) => return std::ptr::null_mut(),
    };

    let fpp = if frames_per_packet == 0 {
        crate::config::DEFAULT_FRAMES_PER_PACKET
    } else {
        frames_per_packet
    };

    let config = EngineConfig {
        target_ip: ip,
        target_port,
        frames_per_packet: fpp,
        ..EngineConfig::default()
    };

    Box::into_raw(Box::new(ZinkosEngine::new(config)))
}

/// Destroy the engine. Safe to call with null.
#[no_mangle]
pub unsafe extern "C" fn zinkos_engine_destroy(engine: *mut ZinkosEngine) {
    if !engine.is_null() {
        let mut engine = Box::from_raw(engine);
        // Stop if running
        if engine.get_state() != State::Stopped {
            let _ = engine.stop();
        }
    }
}

/// Start the engine. Returns 0 on success, -1 on failure.
#[no_mangle]
pub unsafe extern "C" fn zinkos_engine_start(engine: *mut ZinkosEngine) -> i32 {
    if engine.is_null() {
        return -1;
    }
    match (*engine).start() {
        Ok(()) => 0,
        Err(_) => -1,
    }
}

/// Stop the engine. Returns 0 on success, -1 on failure.
#[no_mangle]
pub unsafe extern "C" fn zinkos_engine_stop(engine: *mut ZinkosEngine) -> i32 {
    if engine.is_null() {
        return -1;
    }
    match (*engine).stop() {
        Ok(()) => 0,
        Err(_) => -1,
    }
}

/// Write interleaved i16 PCM frames. RT-safe.
/// Returns number of frames written.
#[no_mangle]
pub unsafe extern "C" fn zinkos_engine_write_frames(
    engine: *mut ZinkosEngine,
    data: *const i16,
    frame_count: u32,
) -> u32 {
    if engine.is_null() || data.is_null() {
        return 0;
    }
    let sample_count = frame_count as usize * 2;
    let slice = std::slice::from_raw_parts(data, sample_count);
    (*engine).write_frames(slice)
}

/// Get current state: 0=Stopped, 1=Starting, 2=Running, 3=Stopping.
#[no_mangle]
pub unsafe extern "C" fn zinkos_engine_get_state(engine: *mut ZinkosEngine) -> u32 {
    if engine.is_null() {
        return 0;
    }
    (*engine).get_state() as u32
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::config::DEFAULT_PORT;
    use std::net::UdpSocket;
    use std::time::Duration;

    fn make_engine(port: u16) -> ZinkosEngine {
        ZinkosEngine::new(EngineConfig {
            target_ip: "127.0.0.1".to_string(),
            target_port: port,
            ring_buffer_frames: 4096,
            ..EngineConfig::default()
        })
    }

    #[test]
    fn create_destroy_cycle() {
        let engine = make_engine(DEFAULT_PORT);
        assert_eq!(engine.get_state(), State::Stopped);
        drop(engine);
    }

    #[test]
    fn start_stop_cycle() {
        let recv = UdpSocket::bind("127.0.0.1:0").unwrap();
        let port = recv.local_addr().unwrap().port();

        let mut engine = make_engine(port);
        engine.start().unwrap();

        // Wait for Running
        for _ in 0..100 {
            if engine.get_state() == State::Running {
                break;
            }
            std::thread::sleep(Duration::from_millis(1));
        }
        assert_eq!(engine.get_state(), State::Running);

        engine.stop().unwrap();
        assert_eq!(engine.get_state(), State::Stopped);
    }

    #[test]
    fn write_while_stopped_returns_zero() {
        let mut engine = make_engine(DEFAULT_PORT);
        let data = [0i16; 480];
        assert_eq!(engine.write_frames(&data), 0);
    }

    #[test]
    fn write_while_running_sends_udp() {
        let recv = UdpSocket::bind("127.0.0.1:0").unwrap();
        let port = recv.local_addr().unwrap().port();
        recv.set_read_timeout(Some(Duration::from_secs(2))).unwrap();

        let mut engine = make_engine(port);
        engine.start().unwrap();

        // Wait for Running
        for _ in 0..100 {
            if engine.get_state() == State::Running {
                break;
            }
            std::thread::sleep(Duration::from_millis(1));
        }

        // Write enough data for one packet (240 frames = 480 samples)
        let data: Vec<i16> = (0..480).map(|i| i as i16).collect();
        let written = engine.write_frames(&data);
        assert!(written > 0, "should have written some frames");

        // Read the UDP packet
        let mut buf = [0u8; 2048];
        let (n, _) = recv.recv_from(&mut buf).unwrap();
        assert!(n > 16, "should receive header + payload, got {n} bytes");

        engine.stop().unwrap();
    }

    #[test]
    fn ffi_null_safety() {
        unsafe {
            assert!(zinkos_engine_create(std::ptr::null(), 4010, 0).is_null());
            assert_eq!(zinkos_engine_start(std::ptr::null_mut()), -1);
            assert_eq!(zinkos_engine_stop(std::ptr::null_mut()), -1);
            assert_eq!(
                zinkos_engine_write_frames(std::ptr::null_mut(), std::ptr::null(), 0),
                0
            );
            assert_eq!(zinkos_engine_get_state(std::ptr::null_mut()), 0);
            // destroy with null should not crash
            zinkos_engine_destroy(std::ptr::null_mut());
        }
    }

    #[test]
    fn ffi_create_start_stop_destroy() {
        let recv = UdpSocket::bind("127.0.0.1:0").unwrap();
        let port = recv.local_addr().unwrap().port();

        unsafe {
            let ip = std::ffi::CString::new("127.0.0.1").unwrap();
            let engine = zinkos_engine_create(ip.as_ptr(), port, 0);
            assert!(!engine.is_null());

            assert_eq!(zinkos_engine_start(engine), 0);

            // Wait for running
            for _ in 0..100 {
                if zinkos_engine_get_state(engine) == 2 {
                    break;
                }
                std::thread::sleep(Duration::from_millis(1));
            }
            assert_eq!(zinkos_engine_get_state(engine), 2);

            assert_eq!(zinkos_engine_stop(engine), 0);
            assert_eq!(zinkos_engine_get_state(engine), 0);

            zinkos_engine_destroy(engine);
        }
    }
}
