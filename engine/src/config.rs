/// Audio format constants — fixed across the entire pipeline.
pub const SAMPLE_RATE: u32 = 48_000;
pub const CHANNELS: u32 = 2;
pub const BYTES_PER_SAMPLE: u32 = 2; // S16_LE
pub const BYTES_PER_FRAME: u32 = CHANNELS * BYTES_PER_SAMPLE; // 4

/// Packet timing defaults
pub const DEFAULT_FRAMES_PER_PACKET: u32 = 240;
pub const HEADER_BYTES: u32 = 20;

/// Protocol constants
pub const PROTO_MAGIC: u16 = 0x5A4B; // "ZK"
pub const PROTO_VERSION: u8 = 1;

/// Ring buffer sizing — enough for ~100ms of audio
pub const RING_BUFFER_FRAMES: usize = 4800;

/// Default network target
pub const DEFAULT_PORT: u16 = 4010;
pub const UDP_SNDBUF_SIZE: u32 = 65536;

/// DSCP Expedited Forwarding (EF) = 46, shifted left 2 for IP_TOS byte
pub const DSCP_EF: u32 = 46 << 2;

pub struct EngineConfig {
    pub target_ip: String,
    pub target_port: u16,
    pub ring_buffer_frames: usize,
    pub frames_per_packet: u32,
}

impl Default for EngineConfig {
    fn default() -> Self {
        Self {
            target_ip: "0.0.0.0".to_string(),
            target_port: DEFAULT_PORT,
            ring_buffer_frames: RING_BUFFER_FRAMES,
            frames_per_packet: DEFAULT_FRAMES_PER_PACKET,
        }
    }
}

impl EngineConfig {
    /// Packet duration in milliseconds, derived from frames_per_packet.
    pub fn packet_duration_ms(&self) -> u64 {
        (self.frames_per_packet as u64 * 1000) / SAMPLE_RATE as u64
    }

    /// Total packet size in bytes (header + payload).
    pub fn packet_bytes(&self) -> usize {
        HEADER_BYTES as usize + self.frames_per_packet as usize * BYTES_PER_FRAME as usize
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn audio_format_constants() {
        assert_eq!(BYTES_PER_FRAME, 4);
        assert_eq!(DEFAULT_FRAMES_PER_PACKET, 240);
    }

    #[test]
    fn default_packet_fits_in_mtu() {
        let cfg = EngineConfig::default();
        assert!(cfg.packet_bytes() < 1400, "packet must fit in typical MTU");
    }

    #[test]
    fn packet_duration_correct() {
        let cfg = EngineConfig::default();
        assert_eq!(cfg.packet_duration_ms(), 5); // 240 frames @ 48kHz = 5ms
    }

    #[test]
    fn ring_buffer_holds_enough() {
        let duration_ms = (RING_BUFFER_FRAMES as f64 / SAMPLE_RATE as f64) * 1000.0;
        assert!(duration_ms >= 90.0, "ring buffer should hold >= 90ms of audio");
    }
}
