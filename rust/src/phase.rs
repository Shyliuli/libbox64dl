//! Explicit engine phase and the owning handle.
//!
//! This is the single writable copy of the engine lifecycle state. C reads it
//! through `box64_is_ready()` / `box64_engine_phase()` and drives the
//! transitions through the short leaf `box64dl_rust_phase_*` exports below; C
//! holds no phase copy of its own and `my_context` is box64's global, never a
//! state source.
//!
//! ## Why the transitions are C-driven
//!
//! The phase owner must not hold a Rust lock or a live Rust frame while a
//! guest-capable C call runs (a foreign `siglongjmp` would skip the Rust
//! frame/guard). So Rust exposes only *prepare* and *commit* leaves:
//!
//! ```text
//!   UNINIT --begin_init--> INITIALIZING --commit_init--> READY
//!                                        \--fail_init--> FAILED (terminal)
//!   READY  --begin_close--> CLOSING      --commit_close--> FINAL (terminal)
//! ```
//!
//! Each leaf takes the phase lock, mutates the state, publishes the phase and
//! returns. The engine calls (`box64dl_core_*`, `endBox64`) happen in C between
//! the leaves, with no Rust frame above them.

use core::ptr::NonNull;
use std::sync::atomic::{AtomicU8, Ordering};

use crate::ffi::c_box64_ctx;
use crate::pt_mutex::PtMutex;

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
#[repr(u8)]
pub enum Phase {
    Uninit = 0,
    Initializing = 1,
    Ready = 2,
    Failed = 3,
    Closing = 4,
    Final = 5,
}

impl Phase {
    fn from_u8(v: u8) -> Phase {
        match v {
            1 => Phase::Initializing,
            2 => Phase::Ready,
            3 => Phase::Failed,
            4 => Phase::Closing,
            5 => Phase::Final,
            _ => Phase::Uninit,
        }
    }

    /// Test/diagnostic accessor: the engine's phase as a stable integer.
    pub fn as_u8(self) -> u8 {
        self as u8
    }
}

/// The Rust-published phase word. C consumes it via `box64_is_ready()` /
/// `box64_engine_phase()`; the storage lives in Rust only (no mirrored atomic
/// layout in C).
static PHASE: AtomicU8 = AtomicU8::new(Phase::Uninit as u8);

pub fn publish(p: Phase) {
    PHASE.store(p.as_u8(), Ordering::Release);
}

pub fn current() -> Phase {
    Phase::from_u8(PHASE.load(Ordering::Acquire))
}

/// Owning identity handle for the C-side context. Never frees (the C shim owns
/// every free path), so dropping it cannot race box64's teardown.
pub struct Engine {
    #[allow(dead_code)] // identity handle: held for ownership, never dereferenced
    ctx: NonNull<c_box64_ctx>,
}

// Safety: the context is a process-wide singleton and every access is
// serialised by the engine lock (`GLOBAL`); the handle is only an identity, so
// moving it between threads is sound.
unsafe impl Send for Engine {}

impl Engine {
    pub fn new(ctx: *mut c_box64_ctx) -> Option<Engine> {
        NonNull::new(ctx).map(|ctx| Engine { ctx })
    }
}

/// The one lifecycle state machine. `Ready` carries the only owning handle;
/// `Failed` and `Final` are terminal and carry no handle. `Initializing` and
/// `Closing` are the in-between states published to C so that a concurrent
/// entry refuses new work instead of entering a half-built/half-torn engine.
pub enum State {
    Uninit,
    Initializing,
    Ready(#[allow(dead_code)] Engine),
    Closing,
    Failed,
    Final,
}

pub static GLOBAL: PtMutex<State> = PtMutex::new(State::Uninit);
