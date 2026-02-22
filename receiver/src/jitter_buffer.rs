use std::collections::VecDeque;
use zinkos_engine::config::{FRAMES_PER_PACKET, SAMPLE_RATE};
use zinkos_engine::packetizer::PacketHeader;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum JitterState {
    /// Accumulating packets before playback starts.
    Filling,
    /// Normal playback.
    Playing,
    /// Buffer ran dry — re-filling before resuming.
    Underrun,
}

/// Entry in the jitter buffer: decoded packet ready for playback.
struct JitterEntry {
    seq: u32,
    frames: Vec<[i16; 2]>,
}

/// Sequence-ordered jitter buffer with configurable start-fill.
///
/// - Receive thread pushes packets via `push()`.
/// - Playback thread pops frames via `pop_frames()`.
/// - Lost packets are gap-filled with silence.
pub struct JitterBuffer {
    buf: VecDeque<JitterEntry>,
    state: JitterState,
    start_fill_frames: usize,
    next_play_seq: Option<u32>,
    // Stats
    pub packets_received: u64,
    pub packets_dropped: u64,
    pub silence_frames_inserted: u64,
    pub underruns: u64,
}

impl JitterBuffer {
    /// Create a new jitter buffer.
    /// `start_fill_ms` is how many milliseconds of audio to buffer before playback begins.
    pub fn new(start_fill_ms: u32) -> Self {
        let start_fill_frames =
            (start_fill_ms as usize * SAMPLE_RATE as usize) / 1000;
        Self {
            buf: VecDeque::new(),
            state: JitterState::Filling,
            start_fill_frames,
            next_play_seq: None,
            packets_received: 0,
            packets_dropped: 0,
            silence_frames_inserted: 0,
            underruns: 0,
        }
    }

    pub fn state(&self) -> JitterState {
        self.state
    }

    /// Total frames currently buffered.
    pub fn buffered_frames(&self) -> usize {
        self.buf.iter().map(|e| e.frames.len()).sum()
    }

    /// Push a received UDP packet into the buffer.
    /// `raw` is the complete packet (header + PCM payload).
    pub fn push(&mut self, raw: &[u8]) {
        if raw.len() < 16 {
            return;
        }

        let hdr = PacketHeader::from_bytes(raw[..16].try_into().unwrap());
        let payload = &raw[16..];
        let expected_bytes = hdr.frames as usize * 4;

        if payload.len() < expected_bytes {
            return;
        }

        // Decode interleaved i16 PCM
        let mut frames = Vec::with_capacity(hdr.frames as usize);
        for i in 0..hdr.frames as usize {
            let offset = i * 4;
            let l = i16::from_le_bytes([payload[offset], payload[offset + 1]]);
            let r = i16::from_le_bytes([payload[offset + 2], payload[offset + 3]]);
            frames.push([l, r]);
        }

        self.packets_received += 1;

        // Drop packets that are too old (behind our play cursor)
        if let Some(next_seq) = self.next_play_seq {
            if Self::seq_before(hdr.seq, next_seq) {
                self.packets_dropped += 1;
                return;
            }
        }

        let entry = JitterEntry {
            seq: hdr.seq,
            frames,
        };

        // Insert in sequence order
        let pos = self
            .buf
            .iter()
            .position(|e| Self::seq_before(hdr.seq, e.seq));
        match pos {
            Some(i) => self.buf.insert(i, entry),
            None => self.buf.push_back(entry),
        }

        // Check if we've filled enough to start playing
        if self.state == JitterState::Filling || self.state == JitterState::Underrun {
            if self.buffered_frames() >= self.start_fill_frames {
                self.state = JitterState::Playing;
                if self.next_play_seq.is_none() {
                    if let Some(first) = self.buf.front() {
                        self.next_play_seq = Some(first.seq);
                    }
                }
            }
        }
    }

    /// Pop up to `count` frames for playback.
    /// Returns the actual frames (may be fewer if underrun, padded with silence for gaps).
    pub fn pop_frames(&mut self, count: usize) -> Vec<[i16; 2]> {
        if self.state != JitterState::Playing {
            return vec![];
        }

        let mut out = Vec::with_capacity(count);
        let mut remaining = count;

        while remaining > 0 {
            if let Some(front) = self.buf.front() {
                let next_seq = self.next_play_seq.unwrap_or(front.seq);

                if front.seq == next_seq {
                    // This is the expected packet
                    let entry = self.buf.pop_front().unwrap();
                    let take = remaining.min(entry.frames.len());
                    out.extend_from_slice(&entry.frames[..take]);
                    remaining -= take;

                    // If we didn't consume the whole packet, push remainder back
                    if take < entry.frames.len() {
                        self.buf.push_front(JitterEntry {
                            seq: entry.seq,
                            frames: entry.frames[take..].to_vec(),
                        });
                    } else {
                        self.next_play_seq = Some(next_seq.wrapping_add(1));
                    }
                } else if Self::seq_before(next_seq, front.seq) {
                    // Gap — insert silence for the missing packet
                    let gap_frames = FRAMES_PER_PACKET as usize;
                    let silence_count = remaining.min(gap_frames);
                    out.extend(std::iter::repeat([0i16; 2]).take(silence_count));
                    remaining -= silence_count;
                    self.silence_frames_inserted += silence_count as u64;
                    self.next_play_seq = Some(next_seq.wrapping_add(1));
                } else {
                    // Packet is behind our cursor — skip it
                    self.buf.pop_front();
                    self.packets_dropped += 1;
                }
            } else {
                // Buffer empty — underrun
                self.state = JitterState::Underrun;
                self.underruns += 1;
                break;
            }
        }

        out
    }

    /// Returns true if `a` comes before `b` in sequence space (handles wrapping).
    fn seq_before(a: u32, b: u32) -> bool {
        // Unsigned subtraction handles wrap-around correctly
        let diff = b.wrapping_sub(a);
        diff > 0 && diff < (u32::MAX / 2)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use zinkos_engine::packetizer::Packetizer;

    fn make_packet(packetizer: &mut Packetizer, frames: &[[i16; 2]]) -> Vec<u8> {
        packetizer.build_packet(frames, 0)
    }

    #[test]
    fn starts_in_filling_state() {
        let jb = JitterBuffer::new(12);
        assert_eq!(jb.state(), JitterState::Filling);
        assert_eq!(jb.buffered_frames(), 0);
    }

    #[test]
    fn transitions_to_playing_after_fill() {
        let mut jb = JitterBuffer::new(5); // 5ms = ~221 frames
        let mut pkt = Packetizer::new();

        let frames = vec![[100i16, 200i16]; FRAMES_PER_PACKET as usize];
        let raw = make_packet(&mut pkt, &frames);
        jb.push(&raw);

        assert_eq!(jb.state(), JitterState::Playing);
        assert_eq!(jb.buffered_frames(), FRAMES_PER_PACKET as usize);
    }

    #[test]
    fn pop_returns_correct_audio() {
        let mut jb = JitterBuffer::new(5);
        let mut pkt = Packetizer::new();

        let frames: Vec<[i16; 2]> = (0..FRAMES_PER_PACKET)
            .map(|i| [i as i16, -(i as i16)])
            .collect();
        let raw = make_packet(&mut pkt, &frames);
        jb.push(&raw);

        let out = jb.pop_frames(10);
        assert_eq!(out.len(), 10);
        assert_eq!(out[0], [0, 0]);
        assert_eq!(out[1], [1, -1]);
        assert_eq!(out[9], [9, -9]);
    }

    #[test]
    fn underrun_when_empty() {
        let mut jb = JitterBuffer::new(5);
        let mut pkt = Packetizer::new();

        let frames = vec![[1i16, 2]; FRAMES_PER_PACKET as usize];
        let raw = make_packet(&mut pkt, &frames);
        jb.push(&raw);
        assert_eq!(jb.state(), JitterState::Playing);

        // Consume all frames
        jb.pop_frames(FRAMES_PER_PACKET as usize);

        // Next pop triggers underrun
        let out = jb.pop_frames(10);
        assert!(out.is_empty());
        assert_eq!(jb.state(), JitterState::Underrun);
        assert_eq!(jb.underruns, 1);
    }

    #[test]
    fn gap_fill_with_silence() {
        let mut jb = JitterBuffer::new(5);
        let mut pkt = Packetizer::new();

        // Push packet 0
        let frames = vec![[10i16, 20]; FRAMES_PER_PACKET as usize];
        let raw0 = make_packet(&mut pkt, &frames);
        jb.push(&raw0);

        // Skip packet 1, push packet 2
        let _raw1 = make_packet(&mut pkt, &frames); // seq 1 — not pushed
        let raw2 = make_packet(&mut pkt, &frames); // seq 2
        jb.push(&raw2);

        // Pop all of packet 0
        let out = jb.pop_frames(FRAMES_PER_PACKET as usize);
        assert_eq!(out.len(), FRAMES_PER_PACKET as usize);
        assert_eq!(out[0], [10, 20]);

        // Next pop should produce silence (gap for missing packet 1)
        let out2 = jb.pop_frames(FRAMES_PER_PACKET as usize);
        assert_eq!(out2.len(), FRAMES_PER_PACKET as usize);
        assert!(out2.iter().all(|f| *f == [0, 0]), "gap should be silence");
        assert!(jb.silence_frames_inserted > 0);
    }

    #[test]
    fn ordering_maintained() {
        // Use 6ms start-fill so both packets are needed to start playback
        // (1 × 240 = 240 frames = 5ms < 6ms, 2 × 240 = 480 frames = 10ms > 6ms)
        let mut jb = JitterBuffer::new(6);
        let mut pkt = Packetizer::new();

        let frames0 = vec![[10i16, 10]; FRAMES_PER_PACKET as usize];
        let frames1 = vec![[20i16, 20]; FRAMES_PER_PACKET as usize];

        let raw0 = make_packet(&mut pkt, &frames0);
        let raw1 = make_packet(&mut pkt, &frames1);

        // Push out of order: packet 1 first, then packet 0
        jb.push(&raw1);
        assert_eq!(jb.state(), JitterState::Filling, "one packet not enough for 10ms");
        jb.push(&raw0);
        assert_eq!(jb.state(), JitterState::Playing);

        // Should play packet 0 first (lower seq number)
        let out = jb.pop_frames(1);
        assert_eq!(out[0], [10, 10]);
    }

    #[test]
    fn underrun_recovery() {
        let mut jb = JitterBuffer::new(5);
        let mut pkt = Packetizer::new();

        let frames = vec![[1i16, 2]; FRAMES_PER_PACKET as usize];
        let raw = make_packet(&mut pkt, &frames);
        jb.push(&raw);

        // Drain to underrun
        jb.pop_frames(FRAMES_PER_PACKET as usize);
        jb.pop_frames(1);
        assert_eq!(jb.state(), JitterState::Underrun);

        // Push new packet — should re-fill and recover
        let raw2 = make_packet(&mut pkt, &frames);
        jb.push(&raw2);
        assert_eq!(jb.state(), JitterState::Playing);

        let out = jb.pop_frames(1);
        assert_eq!(out.len(), 1);
    }

    #[test]
    fn stats_accuracy() {
        let mut jb = JitterBuffer::new(5);
        let mut pkt = Packetizer::new();

        let frames = vec![[0i16; 2]; FRAMES_PER_PACKET as usize];
        for _ in 0..5 {
            let raw = make_packet(&mut pkt, &frames);
            jb.push(&raw);
        }
        assert_eq!(jb.packets_received, 5);
        assert_eq!(jb.packets_dropped, 0);
    }

    #[test]
    fn rejects_short_packets() {
        let mut jb = JitterBuffer::new(12);
        jb.push(&[0u8; 10]); // too short for header
        assert_eq!(jb.packets_received, 0);
    }
}
