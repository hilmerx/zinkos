/// Drift estimator — monitors ring buffer fill level to detect clock drift
/// between CoreAudio's sample clock and our network send clock.
///
/// Uses a sliding window of fill-level samples and linear regression
/// to estimate drift in PPM (parts per million).
///
/// v1: logging/monitoring only. v2: could feed a resampler.

const WINDOW_SIZE: usize = 200; // ~1 second at 5ms sample interval

pub struct DriftEstimator {
    samples: Vec<f64>,
    write_pos: usize,
    filled: bool,
}

impl DriftEstimator {
    pub fn new() -> Self {
        Self {
            samples: vec![0.0; WINDOW_SIZE],
            write_pos: 0,
            filled: false,
        }
    }

    /// Record a fill-level sample (call this every ~5ms, once per packet cycle).
    pub fn push(&mut self, fill_level_frames: usize) {
        self.samples[self.write_pos] = fill_level_frames as f64;
        self.write_pos += 1;
        if self.write_pos >= WINDOW_SIZE {
            self.write_pos = 0;
            self.filled = true;
        }
    }

    /// Estimate drift in PPM using linear regression on the fill-level window.
    /// Positive PPM = producer faster than consumer (fill growing).
    /// Negative PPM = producer slower (fill shrinking).
    /// Returns None if not enough samples yet.
    pub fn estimate_ppm(&self) -> Option<f64> {
        if !self.filled {
            return None;
        }

        // Compute linear regression: slope of fill_level vs time
        let n = WINDOW_SIZE as f64;
        let mut sum_x = 0.0;
        let mut sum_y = 0.0;
        let mut sum_xy = 0.0;
        let mut sum_xx = 0.0;

        for i in 0..WINDOW_SIZE {
            // Reorder samples so index 0 = oldest
            let idx = (self.write_pos + i) % WINDOW_SIZE;
            let x = i as f64;
            let y = self.samples[idx];
            sum_x += x;
            sum_y += y;
            sum_xy += x * y;
            sum_xx += x * x;
        }

        let denom = n * sum_xx - sum_x * sum_x;
        if denom.abs() < 1e-10 {
            return Some(0.0);
        }

        let slope = (n * sum_xy - sum_x * sum_y) / denom;

        // slope is in frames per sample-interval (5ms).
        // Convert to PPM: slope_frames_per_5ms / frames_per_5ms * 1e6
        let frames_per_interval = 221.0; // FRAMES_PER_PACKET
        let ppm = (slope / frames_per_interval) * 1e6;

        Some(ppm)
    }

    /// Returns true if drift exceeds the given threshold (in PPM).
    pub fn is_drifting(&self, threshold_ppm: f64) -> bool {
        self.estimate_ppm()
            .map(|ppm| ppm.abs() > threshold_ppm)
            .unwrap_or(false)
    }

    pub fn is_ready(&self) -> bool {
        self.filled
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn not_ready_until_filled() {
        let mut d = DriftEstimator::new();
        assert!(!d.is_ready());
        assert!(d.estimate_ppm().is_none());

        for i in 0..WINDOW_SIZE {
            d.push(100);
            if i < WINDOW_SIZE - 1 {
                assert!(!d.is_ready());
            }
        }
        assert!(d.is_ready());
        assert!(d.estimate_ppm().is_some());
    }

    #[test]
    fn stable_clock_near_zero_ppm() {
        let mut d = DriftEstimator::new();
        // Constant fill level = no drift
        for _ in 0..WINDOW_SIZE {
            d.push(500);
        }
        let ppm = d.estimate_ppm().unwrap();
        assert!(
            ppm.abs() < 1.0,
            "stable fill should give ~0 ppm, got {ppm}"
        );
    }

    #[test]
    fn increasing_fill_positive_ppm() {
        let mut d = DriftEstimator::new();
        // Fill level increasing = producer faster
        for i in 0..WINDOW_SIZE {
            d.push(500 + i);
        }
        let ppm = d.estimate_ppm().unwrap();
        assert!(ppm > 0.0, "increasing fill should give positive ppm, got {ppm}");
    }

    #[test]
    fn decreasing_fill_negative_ppm() {
        let mut d = DriftEstimator::new();
        // Fill level decreasing = producer slower
        for i in 0..WINDOW_SIZE {
            d.push(1000 - i);
        }
        let ppm = d.estimate_ppm().unwrap();
        assert!(ppm < 0.0, "decreasing fill should give negative ppm, got {ppm}");
    }

    #[test]
    fn threshold_detection() {
        let mut d = DriftEstimator::new();
        for i in 0..WINDOW_SIZE {
            d.push(500 + i * 10); // rapid increase
        }
        assert!(d.is_drifting(100.0), "large drift should exceed threshold");

        let mut d2 = DriftEstimator::new();
        for _ in 0..WINDOW_SIZE {
            d2.push(500);
        }
        assert!(!d2.is_drifting(100.0), "stable should not exceed threshold");
    }
}
