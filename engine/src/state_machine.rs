use std::sync::atomic::{AtomicU8, Ordering};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub enum State {
    Stopped = 0,
    Starting = 1,
    Running = 2,
    Stopping = 3,
}

impl State {
    fn from_u8(v: u8) -> Option<Self> {
        match v {
            0 => Some(State::Stopped),
            1 => Some(State::Starting),
            2 => Some(State::Running),
            3 => Some(State::Stopping),
            _ => None,
        }
    }
}

/// Lock-free state machine for the engine lifecycle.
///
/// Transition diagram:
///   Stopped → Starting → Running → Stopping → Stopped
///
/// The CoreAudio RT thread only reads state (via `get()` / `is_accepting_audio()`).
/// The control thread drives transitions.
pub struct StateMachine {
    state: AtomicU8,
}

impl StateMachine {
    pub fn new() -> Self {
        Self {
            state: AtomicU8::new(State::Stopped as u8),
        }
    }

    pub fn get(&self) -> State {
        State::from_u8(self.state.load(Ordering::Acquire)).unwrap_or(State::Stopped)
    }

    /// Returns true if the engine should accept audio frames (RT-safe read).
    pub fn is_accepting_audio(&self) -> bool {
        matches!(self.get(), State::Starting | State::Running)
    }

    /// Attempt a state transition. Returns Ok(new_state) or Err(current_state).
    fn transition(&self, from: State, to: State) -> Result<State, State> {
        match self.state.compare_exchange(
            from as u8,
            to as u8,
            Ordering::AcqRel,
            Ordering::Acquire,
        ) {
            Ok(_) => Ok(to),
            Err(actual) => Err(State::from_u8(actual).unwrap_or(State::Stopped)),
        }
    }

    /// Stopped → Starting. Idempotent if already Starting/Running.
    pub fn start(&self) -> Result<State, State> {
        match self.get() {
            State::Starting | State::Running => Ok(self.get()),
            _ => self.transition(State::Stopped, State::Starting),
        }
    }

    /// Starting → Running. Called once the network thread is ready.
    pub fn mark_running(&self) -> Result<State, State> {
        self.transition(State::Starting, State::Running)
    }

    /// Running → Stopping. Idempotent if already Stopping/Stopped.
    pub fn stop(&self) -> Result<State, State> {
        match self.get() {
            State::Stopping | State::Stopped => Ok(self.get()),
            _ => self.transition(State::Running, State::Stopping),
        }
    }

    /// Stopping → Stopped. Called once the network thread has exited.
    pub fn mark_stopped(&self) -> Result<State, State> {
        self.transition(State::Stopping, State::Stopped)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::Arc;

    #[test]
    fn initial_state_is_stopped() {
        let sm = StateMachine::new();
        assert_eq!(sm.get(), State::Stopped);
        assert!(!sm.is_accepting_audio());
    }

    #[test]
    fn full_lifecycle() {
        let sm = StateMachine::new();

        sm.start().unwrap();
        assert_eq!(sm.get(), State::Starting);
        assert!(sm.is_accepting_audio());

        sm.mark_running().unwrap();
        assert_eq!(sm.get(), State::Running);
        assert!(sm.is_accepting_audio());

        sm.stop().unwrap();
        assert_eq!(sm.get(), State::Stopping);
        assert!(!sm.is_accepting_audio());

        sm.mark_stopped().unwrap();
        assert_eq!(sm.get(), State::Stopped);
    }

    #[test]
    fn start_is_idempotent() {
        let sm = StateMachine::new();
        sm.start().unwrap();
        // Calling start again while Starting should succeed
        assert!(sm.start().is_ok());
        assert_eq!(sm.get(), State::Starting);

        sm.mark_running().unwrap();
        // Calling start while Running should also succeed (idempotent)
        assert!(sm.start().is_ok());
        assert_eq!(sm.get(), State::Running);
    }

    #[test]
    fn stop_is_idempotent() {
        let sm = StateMachine::new();
        sm.start().unwrap();
        sm.mark_running().unwrap();
        sm.stop().unwrap();
        // Calling stop again while Stopping should succeed
        assert!(sm.stop().is_ok());
        assert_eq!(sm.get(), State::Stopping);
    }

    #[test]
    fn invalid_transition_from_stopped() {
        let sm = StateMachine::new();
        // Can't go directly to Running from Stopped
        assert!(sm.mark_running().is_err());
        // Stop from Stopped is idempotent (not an error)
        assert!(sm.stop().is_ok());
        assert_eq!(sm.get(), State::Stopped);
    }

    #[test]
    fn invalid_transition_from_starting() {
        let sm = StateMachine::new();
        sm.start().unwrap();
        // Can't stop from Starting (must be Running first)
        assert!(sm.stop().is_err());
    }

    #[test]
    fn concurrent_access() {
        let sm = Arc::new(StateMachine::new());
        sm.start().unwrap();
        sm.mark_running().unwrap();

        let handles: Vec<_> = (0..8)
            .map(|_| {
                let sm = Arc::clone(&sm);
                std::thread::spawn(move || {
                    for _ in 0..10_000 {
                        let _ = sm.is_accepting_audio();
                        let _ = sm.get();
                    }
                })
            })
            .collect();

        for h in handles {
            h.join().unwrap();
        }

        assert_eq!(sm.get(), State::Running);
    }
}
