/// ALSA PCM output sink for the Raspberry Pi.
///
/// This module only compiles on Linux (where ALSA is available).
/// On macOS, we provide a stub for testing the receiver logic.

#[cfg(target_os = "linux")]
mod platform {
    use alsa::pcm::{Access, Format, HwParams, PCM};
    use alsa::{Direction, ValueOr};
    use zinkos_engine::config::{CHANNELS, FRAMES_PER_PACKET, SAMPLE_RATE};

    pub struct AlsaSink {
        pcm: PCM,
    }

    impl AlsaSink {
        pub fn open(device: &str) -> Result<Self, String> {
            let pcm = PCM::new(device, Direction::Playback, false)
                .map_err(|e| format!("ALSA open '{device}': {e}"))?;

            {
                let hwp = HwParams::any(&pcm)
                    .map_err(|e| format!("ALSA HwParams: {e}"))?;
                hwp.set_channels(CHANNELS)
                    .map_err(|e| format!("ALSA set_channels: {e}"))?;
                hwp.set_rate(SAMPLE_RATE, ValueOr::Nearest)
                    .map_err(|e| format!("ALSA set_rate: {e}"))?;
                hwp.set_format(Format::s16())
                    .map_err(|e| format!("ALSA set_format: {e}"))?;
                hwp.set_access(Access::RWInterleaved)
                    .map_err(|e| format!("ALSA set_access: {e}"))?;
                hwp.set_period_size(FRAMES_PER_PACKET as i64, ValueOr::Nearest)
                    .map_err(|e| format!("ALSA set_period_size: {e}"))?;
                hwp.set_buffer_size((FRAMES_PER_PACKET as i64) * 3)
                    .map_err(|e| format!("ALSA set_buffer_size: {e}"))?;
                pcm.hw_params(&hwp)
                    .map_err(|e| format!("ALSA hw_params: {e}"))?;
            }

            // sw_params: control when ALSA starts playback
            {
                let swp = pcm.sw_params_current()
                    .map_err(|e| format!("ALSA sw_params_current: {e}"))?;
                // Start hardware after 2 periods are written (~10ms) — not 1 frame (underrun)
                // and not buffer_size (adds full buffer latency)
                swp.set_start_threshold(FRAMES_PER_PACKET as i64 * 2)
                    .map_err(|e| format!("ALSA set_start_threshold: {e}"))?;
                // Wake us from writei block when 1 period of space is available
                swp.set_avail_min(FRAMES_PER_PACKET as i64)
                    .map_err(|e| format!("ALSA set_avail_min: {e}"))?;
                pcm.sw_params(&swp)
                    .map_err(|e| format!("ALSA sw_params: {e}"))?;
            }

            Ok(Self { pcm })
        }

        /// Write interleaved stereo frames to ALSA.
        /// Each frame is [i16; 2] = [left, right].
        pub fn write_frames(&self, frames: &[[i16; 2]]) -> Result<usize, String> {
            // Convert [i16; 2] frames to flat &[i16] for ALSA
            let flat: &[i16] = unsafe {
                std::slice::from_raw_parts(
                    frames.as_ptr() as *const i16,
                    frames.len() * 2,
                )
            };

            let io = self.pcm.io_i16()
                .map_err(|e| format!("ALSA io: {e}"))?;

            match io.writei(flat) {
                Ok(n) => Ok(n),
                Err(e) => {
                    // Try to recover from underrun
                    eprintln!("ALSA write error: {e}, attempting recovery");
                    self.pcm.recover(e.errno() as i32, true)
                        .map_err(|e| format!("ALSA recover: {e}"))?;
                    Ok(0)
                }
            }
        }
    }
}

#[cfg(not(target_os = "linux"))]
mod platform {
    /// Stub AlsaSink for macOS development/testing.
    /// Discards audio but tracks frame count for verification.
    pub struct AlsaSink {
        pub frames_written: u64,
    }

    impl AlsaSink {
        pub fn open(device: &str) -> Result<Self, String> {
            eprintln!("AlsaSink stub: would open '{device}' (not on Linux)");
            Ok(Self { frames_written: 0 })
        }

        pub fn write_frames(&mut self, frames: &[[i16; 2]]) -> Result<usize, String> {
            self.frames_written += frames.len() as u64;
            Ok(frames.len())
        }
    }
}

pub use platform::AlsaSink;
