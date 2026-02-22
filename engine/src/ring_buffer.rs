use rtrb::{Consumer, Producer, RingBuffer as RtrbRingBuffer};

/// A stereo audio frame: [left, right].
pub type Frame = [i16; 2];

/// RT-safe ring buffer wrapping `rtrb`.
///
/// Producer side (CoreAudio RT thread): `write_frames` — never allocates, never blocks.
/// Consumer side (network thread): `read_frames` — reads available frames for packetization.
pub struct RingBuffer {
    producer: Producer<Frame>,
    consumer: Consumer<Frame>,
    capacity: usize,
}

impl RingBuffer {
    pub fn new(capacity_frames: usize) -> Self {
        let (producer, consumer) = RtrbRingBuffer::new(capacity_frames);
        Self {
            producer,
            consumer,
            capacity: capacity_frames,
        }
    }

    /// Split into producer/consumer halves for separate threads.
    pub fn split(self) -> (RingProducer, RingConsumer) {
        (
            RingProducer {
                inner: self.producer,
                capacity: self.capacity,
            },
            RingConsumer {
                inner: self.consumer,
                capacity: self.capacity,
            },
        )
    }
}

/// Producer half — owned by the RT thread. All operations are wait-free.
pub struct RingProducer {
    inner: Producer<Frame>,
    capacity: usize,
}

impl RingProducer {
    /// Write interleaved i16 samples into the ring buffer.
    /// Input: &[i16] with alternating L, R, L, R, ...
    /// Returns the number of *frames* written (may be less if buffer is full).
    /// This is RT-safe: no allocation, no lock, no syscall.
    pub fn write_frames(&mut self, interleaved: &[i16]) -> usize {
        let frame_count = interleaved.len() / 2;
        let mut written = 0;

        for i in 0..frame_count {
            let frame = [interleaved[i * 2], interleaved[i * 2 + 1]];
            match self.inner.push(frame) {
                Ok(()) => written += 1,
                Err(_) => break, // buffer full — drop frames (RT-safe choice)
            }
        }

        written
    }

    /// Number of frames that can be written without dropping.
    pub fn available(&self) -> usize {
        self.inner.slots()
    }

    pub fn capacity(&self) -> usize {
        self.capacity
    }
}

/// Consumer half — owned by the network thread.
pub struct RingConsumer {
    inner: Consumer<Frame>,
    capacity: usize,
}

impl RingConsumer {
    /// Read up to `out.len()` frames. Returns the number actually read.
    pub fn read_frames(&mut self, out: &mut [Frame]) -> usize {
        let mut read = 0;
        for slot in out.iter_mut() {
            match self.inner.pop() {
                Ok(frame) => {
                    *slot = frame;
                    read += 1;
                }
                Err(_) => break,
            }
        }
        read
    }

    /// Number of frames currently available to read.
    pub fn fill_level(&self) -> usize {
        self.inner.slots()
    }

    pub fn capacity(&self) -> usize {
        self.capacity
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn round_trip() {
        let rb = RingBuffer::new(1024);
        let (mut prod, mut cons) = rb.split();

        let input = [100i16, -200, 300, -400, 500, -600];
        let written = prod.write_frames(&input);
        assert_eq!(written, 3);

        let mut out = [[0i16; 2]; 3];
        let read = cons.read_frames(&mut out);
        assert_eq!(read, 3);
        assert_eq!(out[0], [100, -200]);
        assert_eq!(out[1], [300, -400]);
        assert_eq!(out[2], [500, -600]);
    }

    #[test]
    fn overrun_drops_excess() {
        let rb = RingBuffer::new(4);
        let (mut prod, _cons) = rb.split();

        // Try to write more than capacity
        let input: Vec<i16> = (0..20).collect();
        let written = prod.write_frames(&input);
        // rtrb usable capacity is (capacity - 1) for SPSC, or exactly capacity
        // depending on version. Just check we didn't write all 10 frames.
        assert!(written <= 4);
    }

    #[test]
    fn underrun_returns_zero() {
        let rb = RingBuffer::new(1024);
        let (_prod, mut cons) = rb.split();

        let mut out = [[0i16; 2]; 10];
        let read = cons.read_frames(&mut out);
        assert_eq!(read, 0);
    }

    #[test]
    fn fill_level_accurate() {
        let rb = RingBuffer::new(1024);
        let (mut prod, cons) = rb.split();

        assert_eq!(cons.fill_level(), 0);

        let input = [1i16, 2, 3, 4, 5, 6]; // 3 frames
        prod.write_frames(&input);
        assert_eq!(cons.fill_level(), 3);
    }

    #[test]
    fn wrap_around() {
        let rb = RingBuffer::new(8);
        let (mut prod, mut cons) = rb.split();

        // Fill and drain a few times to exercise wrap-around
        for round in 0..5 {
            let val = (round * 100) as i16;
            let input = [val, val + 1, val + 2, val + 3];
            let written = prod.write_frames(&input);
            assert_eq!(written, 2, "round {round}");

            let mut out = [[0i16; 2]; 2];
            let read = cons.read_frames(&mut out);
            assert_eq!(read, 2, "round {round}");
            assert_eq!(out[0], [val, val + 1]);
            assert_eq!(out[1], [val + 2, val + 3]);
        }
    }

    #[test]
    fn interleaved_conversion() {
        let rb = RingBuffer::new(1024);
        let (mut prod, mut cons) = rb.split();

        // Simulate what CoreAudio delivers: flat interleaved buffer
        let interleaved: Vec<i16> = (0..480).map(|i| i as i16).collect();
        let written = prod.write_frames(&interleaved);
        assert_eq!(written, 240); // exactly one packet's worth

        let mut out = [[0i16; 2]; 240];
        let read = cons.read_frames(&mut out);
        assert_eq!(read, 240);

        // Verify interleaving
        for i in 0..240 {
            assert_eq!(out[i][0], (i * 2) as i16);
            assert_eq!(out[i][1], (i * 2 + 1) as i16);
        }
    }

    #[test]
    fn threaded_stress() {
        use std::sync::Arc;
        use std::sync::atomic::{AtomicBool, Ordering};

        let rb = RingBuffer::new(4096);
        let (mut prod, mut cons) = rb.split();
        let done = Arc::new(AtomicBool::new(false));

        let total_frames = 48000; // 1 second of audio
        let done_w = Arc::clone(&done);

        let writer = std::thread::spawn(move || {
            let mut total_written = 0usize;
            let chunk: Vec<i16> = (0..480).map(|i| i as i16).collect(); // 240 frames

            while total_written < total_frames {
                let w = prod.write_frames(&chunk);
                total_written += w;
                if w == 0 {
                    std::thread::yield_now();
                }
            }
            done_w.store(true, Ordering::Release);
            total_written
        });

        let reader = std::thread::spawn(move || {
            let mut total_read = 0usize;
            let mut buf = [[0i16; 2]; 256];

            loop {
                let r = cons.read_frames(&mut buf);
                total_read += r;
                if r == 0 {
                    if done.load(Ordering::Acquire) {
                        // Drain remaining
                        loop {
                            let r = cons.read_frames(&mut buf);
                            if r == 0 {
                                break;
                            }
                            total_read += r;
                        }
                        break;
                    }
                    std::thread::yield_now();
                }
            }
            total_read
        });

        let written = writer.join().unwrap();
        let read = reader.join().unwrap();
        assert_eq!(written, read, "all frames written must be read");
    }
}
