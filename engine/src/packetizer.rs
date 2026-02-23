use crate::config::{FRAMES_PER_PACKET, HEADER_BYTES, PROTO_MAGIC, PROTO_VERSION};

#[cfg(test)]
use crate::config::PACKET_BYTES;

/// Wire format for Zinkos UDP packets.
/// All fields are little-endian (both Mac ARM and Pi 5 ARM are LE).
/// Wire-safe: we serialize manually via to_bytes/from_bytes,
/// so the in-memory layout doesn't need to match wire format.
///
/// Wire layout (20 bytes):
///   [0..2]   magic: u16        — 0x5A4B ("ZK")
///   [2]      version: u8       — protocol version (currently 1)
///   [3]      flags: u8         — reserved (0)
///   [4..8]   seq: u32          — sequence number
///   [8..12]  frames: u32       — frame count in this packet
///   [12..20] timestamp_ns: u64 — nanoseconds since stream start
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct PacketHeader {
    pub version: u8,
    pub flags: u8,
    pub seq: u32,
    pub frames: u32,
    pub timestamp_ns: u64,
}

impl PacketHeader {
    pub fn to_bytes(&self) -> [u8; 20] {
        let mut buf = [0u8; 20];
        buf[0..2].copy_from_slice(&PROTO_MAGIC.to_le_bytes());
        buf[2] = self.version;
        buf[3] = self.flags;
        buf[4..8].copy_from_slice(&self.seq.to_le_bytes());
        buf[8..12].copy_from_slice(&self.frames.to_le_bytes());
        buf[12..20].copy_from_slice(&self.timestamp_ns.to_le_bytes());
        buf
    }

    pub fn from_bytes(buf: &[u8; 20]) -> Option<Self> {
        let magic = u16::from_le_bytes([buf[0], buf[1]]);
        if magic != PROTO_MAGIC {
            return None;
        }
        Some(Self {
            version: buf[2],
            flags: buf[3],
            seq: u32::from_le_bytes(buf[4..8].try_into().unwrap()),
            frames: u32::from_le_bytes(buf[8..12].try_into().unwrap()),
            timestamp_ns: u64::from_le_bytes(buf[12..20].try_into().unwrap()),
        })
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

    /// Build a packet from up to `FRAMES_PER_PACKET` frames of interleaved i16 PCM.
    /// Returns the full packet bytes (header + payload).
    pub fn build_packet(&mut self, pcm_frames: &[[i16; 2]], timestamp_ns: u64) -> Vec<u8> {
        assert!(
            pcm_frames.len() <= FRAMES_PER_PACKET as usize,
            "too many frames for one packet"
        );

        let frame_count = pcm_frames.len() as u32;
        let payload_size = frame_count as usize * 4; // 2 channels × 2 bytes

        let header = PacketHeader {
            version: PROTO_VERSION,
            flags: 0,
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
        let h = PacketHeader {
            version: PROTO_VERSION,
            flags: 0,
            seq: 0,
            frames: 0,
            timestamp_ns: 0,
        };
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
            version: PROTO_VERSION,
            flags: 0,
            seq: 42,
            frames: 221,
            timestamp_ns: 1_000_000_000,
        };
        let bytes = h.to_bytes();
        let h2 = PacketHeader::from_bytes(&bytes).expect("valid header");
        assert_eq!(h2.version, PROTO_VERSION);
        assert_eq!(h2.seq, 42);
        assert_eq!(h2.frames, 221);
        assert_eq!(h2.timestamp_ns, 1_000_000_000);
    }

    #[test]
    fn rejects_bad_magic() {
        let mut bytes = [0u8; 20];
        bytes[0] = 0xFF;
        bytes[1] = 0xFF;
        assert!(PacketHeader::from_bytes(&bytes).is_none());
    }

    #[test]
    fn magic_bytes_correct() {
        let h = PacketHeader {
            version: PROTO_VERSION,
            flags: 0,
            seq: 0,
            frames: 0,
            timestamp_ns: 0,
        };
        let bytes = h.to_bytes();
        assert_eq!(bytes[0], 0x4B); // 'K' (LE: low byte first)
        assert_eq!(bytes[1], 0x5A); // 'Z'
        assert_eq!(bytes[2], PROTO_VERSION);
        assert_eq!(bytes[3], 0); // flags
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
        let hdr = PacketHeader::from_bytes(pkt[..20].try_into().unwrap()).unwrap();
        assert_eq!(hdr.seq, 0);
        assert_eq!(hdr.frames, 3);
        assert_eq!(hdr.timestamp_ns, 99);

        // Verify PCM payload
        let payload = &pkt[20..];
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
