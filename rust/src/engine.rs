//! Engine phase leaves: the Rust half of the prepare / C-execute / commit
//! lifecycle.
//!
//! Every export here is a SHORT LEAF: it takes the phase lock, mutates the
//! single Rust `State`, publishes the phase word and returns. It never calls a
//! C engine function, so no Rust frame or guard is live while C runs
//! `box64dl_core_*` / `endBox64` (which can run guest code and be crossed by a
//! foreign `siglongjmp`).
//!
//! C (`src/libbox64.c`) owns the orchestration. The caller quiesces all
//! engine work before fini; no per-call admission gate protects concurrent
//! teardown:
//!
//! ```text
//!   box64_lib_init:  begin_init -> [C: engine_boot] -> commit_init(x) | fail_init
//!   box64_lib_fini:  begin_close -> [C: endBox64] -> commit_close
//!   box64_ensure_emu: READY check -> [C: create emu if needed]
//! ```
//!
//! The status codes are stable so C can put the right reason in the error slot:
//! `0` = you own the transition, `1` = already READY (init is idempotent),
//! `-2` = initialization already in progress, `-3` = FAILED (terminal),
//! `-4` = CLOSING/FINAL (terminal).

use core::ffi::c_void;

use crate::phase::{Engine, GLOBAL, Phase, State, publish};

/// Start initialization. See the module doc for the status codes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn box64dl_rust_phase_begin_init() -> i32 {
    let mut g = GLOBAL.lock();
    match &*g {
        State::Ready(_) => 1,
        State::Initializing => -2,
        State::Failed => -3,
        State::Closing | State::Final => -4,
        State::Uninit => {
            *g = State::Initializing;
            publish(Phase::Initializing);
            0
        }
    }
}

/// Commit a successful initialization with the C-owned context pointer.
/// Returns 0 and publishes READY, or -1 and publishes FAILED for a NULL handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn box64dl_rust_phase_commit_init(ctx: *mut c_void) -> i32 {
    let mut g = GLOBAL.lock();
    if !matches!(&*g, State::Initializing) {
        return -1;
    }
    match Engine::new(ctx as *mut crate::ffi::c_box64_ctx) {
        Some(engine) => {
            *g = State::Ready(engine);
            publish(Phase::Ready);
            0
        }
        None => {
            *g = State::Failed;
            publish(Phase::Failed);
            -1
        }
    }
}

/// Fail initialization: INITIALIZING -> FAILED (terminal). No-op otherwise.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn box64dl_rust_phase_fail_init() {
    let mut g = GLOBAL.lock();
    if matches!(&*g, State::Initializing) {
        *g = State::Failed;
        publish(Phase::Failed);
    }
}

/// Enter closing: READY -> CLOSING and drop the owning handle. Returns 0 when
/// this call owns the teardown, -1 when the engine was not READY (no-op).
/// Publishing CLOSING is what makes every concurrent entry refuse new work
/// before `endBox64()` touches the context.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn box64dl_rust_phase_begin_close() -> i32 {
    let mut g = GLOBAL.lock();
    if !matches!(&*g, State::Ready(_)) {
        return -1;
    }
    *g = State::Closing;
    publish(Phase::Closing);
    0
}

/// Finish closing: CLOSING -> FINAL (terminal). No-op otherwise.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn box64dl_rust_phase_commit_close() {
    let mut g = GLOBAL.lock();
    if matches!(&*g, State::Closing) {
        *g = State::Final;
        publish(Phase::Final);
    }
}

pub fn is_ready() -> bool {
    crate::phase::current() == Phase::Ready
}
