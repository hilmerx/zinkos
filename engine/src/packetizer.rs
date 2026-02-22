use crate::config::{FRAMES_PER_PACKET, HEADER_BYTES};

#[cfg(test)]
use crate::config::PACKET_BYTES;

/// Wire format for Zinkos UDP packets.
/// All fields are little-endian (both Mac ARM and Pi 5 ARM are LE).
/// Wire-safe: we serialize manually via to_bytes/from_bytes,
/// so the in-memory layout doesn't need to match wire format.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct PacketHeader {
    pub seq: u32,
    pub frames: u32,
    pub timestamp_ns: u64,
}

impl PacketHeader {
    pub fn to_bytes(&self) -> [u8; 16] {
        let mut buf = [0u8; 16];
        buf[0..4].copy_from_slice(&self.seq.to_le_bytes());
        buf[4..8].copy_from_slice(&self.frames.to_le_bytes());
        buf[8..16].copy_from_slice(&self.timestamp_ns.to_le_bytes());
        buf
    }

    pub fn from_bytes(buf: &[u8; 16]) -> Self {
        Self {
            seq: u32::from_le_bytes(buf[0..4].try_into().unwrap()),
            frames: u32::from_le_bytes(buf[4..8].try_into().unwrap()),
            timestamp_ns: u64::from_le_bytes(buf[8..16].try_into().unwrap()),
        }
    }
}

/// Builds complete UDP packets from PCM frames.
pub struct Packetizer {
    seq: u32,
}

impl Packetizer {
    pub fn new() -> Self {
        Self { seq: 0 }
    }

    /// Build a packet from exactly `FRAMES_PER_PACKET` frames of interleaved i16 PCM.
    /// Returns the full packet bytes (header + payload).
    pub fn build_packet(&mut self, pcm_frames: &[[i16; 2]], timestamp_ns: u64) -> Vec<u8> {
        assert!(
            pcm_frames.len() <= FRAMES_PER_PACKET as usize,
            "too many frames for one packet"
        );

        let frame_count = pcm_frames.len() as u32;
        let payload_size = frame_count as usize * 4; // 2 channels × 2 bytes

        let header = PacketHeader {
            seq: self.seq,
            frames: frame_count,
            timestamp_ns,
        };
        self.seq = self.seq.wrapping_add(1);

        let mut packet = Vec::with_capacity(HEADER_BYTES as usize + payload_size);
        packet.extend_from_slice(&header.to_bytes());

        for frame in pcm_frames {
            packet.extend_from_slice(&frame[0].to_le_bytes());
            packet.extend_from_slice(&frame[1].to_le_bytes());
        }

        packet
    }

    pub fn seq(&self) -> u32 {
        self.seq
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn header_wire_size() {
        let h = PacketHeader { seq: 0, frames: 0, timestamp_ns: 0 };
        assert_eq!(h.to_bytes().len(), HEADER_BYTES as usize);
    }

    #[test]
    fn full_packet_size() {
        let mut p = Packetizer::new();
        let frames = vec![[0i16; 2]; FRAMES_PER_PACKET as usize];
        let pkt = p.build_packet(&frames, 0);
        assert_eq!(pkt.len(), PACKET_BYTES as usize);
    }

    #[test]
    fn header_round_trip() {
        let h = PacketHeader {
            seq: 42,
            frames: 221,
            timestamp_ns: 1_000_000_000,
        };
        let bytes = h.to_bytes();
        let h2 = PacketHeader::from_bytes(&bytes);
        assert_eq!(h2.seq, 42);
        assert_eq!(h2.frames, 221);
        assert_eq!(h2.timestamp_ns, 1_000_000_000);
    }

    #[test]
    fn sequence_increments() {
        let mut p = Packetizer::new();
        let frames = vec![[0i16; 2]; 10];
        p.build_packet(&frames, 0);
        p.build_packet(&frames, 0);
        p.build_packet(&frames, 0);
        assert_eq!(p.seq(), 3);
    }

    #[test]
    fn sequence_wraps() {
        let mut p = Packetizer { seq: u32::MAX };
        let frames = vec![[0i16; 2]; 10];
        p.build_packet(&frames, 0);
        assert_eq!(p.seq(), 0);
    }

    #[test]
    fn pcm_data_preserved() {
        let mut p = Packetizer::new();
        let frames = vec![[1234i16, -5678i16]; 3];
        let pkt = p.build_packet(&frames, 99);

        // Verify header
        let hdr = PacketHeader::from_bytes(pkt[..16].try_into().unwrap());
        assert_eq!(hdr.seq, 0);
        assert_eq!(hdr.frames, 3);
        assert_eq!(hdr.timestamp_ns, 99);

        // Verify PCM payload
        let payload = &pkt[16..];
        assert_eq!(payload.len(), 12); // 3 frames × 4 bytes
        let l = i16::from_le_bytes([payload[0], payload[1]]);
        let r = i16::from_le_bytes([payload[2], payload[3]]);
        assert_eq!(l, 1234);
        assert_eq!(r, -5678);
    }

    #[test]
    fn partial_packet_smaller() {
        let mut p = Packetizer::new();
        let frames = vec![[0i16; 2]; 100];
        let pkt = p.build_packet(&frames, 0);
        assert_eq!(pkt.len(), HEADER_BYTES as usize + 100 * 4);
        assert!(pkt.len() < PACKET_BYTES as usize);
    }
}
