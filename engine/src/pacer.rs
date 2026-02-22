//! High-precision packet pacer.
//!
//! Uses `mach_wait_until` on macOS for sub-microsecond accuracy.
//! Falls back to `thread::sleep` on other platforms.
//! Absolute deadlines prevent drift accumulation over time.

use std::time::Duration;

pub struct Pacer {
    #[cfg(target_os = "macos")]
    next_abs: u64,
    #[cfg(target_os = "macos")]
    interval_abs: u64,

    #[cfg(not(target_os = "macos"))]
    next: Option<std::time::Instant>,
    #[cfg(not(target_os = "macos"))]
    interval: Duration,
}

#[cfg(target_os = "macos")]
#[repr(C)]
struct MachTimebaseInfo {
    numer: u32,
    denom: u32,
}

#[cfg(target_os = "macos")]
extern "C" {
    fn mach_absolute_time() -> u64;
    fn mach_wait_until(deadline: u64) -> u32;
    fn mach_timebase_info(info: *mut MachTimebaseInfo) -> i32;
}

impl Pacer {
    pub fn new(interval: Duration) -> Self {
        #[cfg(target_os = "macos")]
        {
            let mut info = MachTimebaseInfo { numer: 0, denom: 0 };
            unsafe {
                mach_timebase_info(&mut info);
            }
            let interval_ns = interval.as_nanos() as u64;
            let interval_abs = interval_ns * info.denom as u64 / info.numer as u64;
            Self {
                next_abs: 0, // initialized on first wait()
                interval_abs,
            }
        }
        #[cfg(not(target_os = "macos"))]
        {
            Self {
                next: None,
                interval,
            }
        }
    }

    /// Block until the next deadline, then advance it.
    /// First call returns immediately and sets the baseline.
    pub fn wait(&mut self) {
        #[cfg(target_os = "macos")]
        {
            if self.next_abs == 0 {
                // First call — set baseline, return immediately
                self.next_abs = unsafe { mach_absolute_time() } + self.interval_abs;
                return;
            }
            unsafe {
                mach_wait_until(self.next_abs);
            }
            self.next_abs += self.interval_abs;
        }
        #[cfg(not(target_os = "macos"))]
        {
            match self.next {
                None => {
                    self.next = Some(std::time::Instant::now() + self.interval);
                }
                Some(deadline) => {
                    let now = std::time::Instant::now();
                    if deadline > now {
                        std::thread::sleep(deadline - now);
                    }
                    self.next = Some(deadline + self.interval);
                }
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::time::Instant;

    #[test]
    fn first_call_returns_immediately() {
        let mut p = Pacer::new(Duration::from_millis(100));
        let start = Instant::now();
        p.wait();
        assert!(start.elapsed() < Duration::from_millis(5));
    }

    #[test]
    fn pacing_accuracy() {
        let mut p = Pacer::new(Duration::from_millis(5));
        p.wait(); // first call, returns immediately

        let start = Instant::now();
        for _ in 0..4 {
            p.wait();
        }
        let elapsed = start.elapsed();
        // 4 × 5ms = 20ms expected, allow ±5ms
        assert!(
            elapsed >= Duration::from_millis(15) && elapsed <= Duration::from_millis(30),
            "expected ~20ms, got {:?}",
            elapsed
        );
    }
}
