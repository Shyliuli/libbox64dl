//! libbox64 — Rust engine phase owner and FFI boundary.
//!
//! Layout:
//!   phase.rs   explicit engine phase + owning context handle (single writer)
//!   engine.rs  short-leaf phase transitions driven by C
//!   error.rs   thread-local error slot (dlerror semantics)
//!   ffi.rs     bindgen bindings for the private C shim only
//!   pt_mutex/  recursive pthread lock primitive
//!
//! The phase owner is Rust, but the lifecycle *orchestration* and the engine
//! calls are C (`src/libbox64.c`): `box64_lib_init` / `box64_lib_fini` /
//! `box64_ensure_emu` are C exports that bracket the guest-capable engine work
//! with the Rust `box64dl_rust_phase_*` leaves. No Rust frame or guard is live
//! while a guest-capable C call runs, so a foreign `siglongjmp` cannot skip a
//! Rust frame.
//!
//! The public C ABI exported from here is deliberately small and pure:
//!   box64_is_ready / box64_engine_phase (phase reads)
//!   box64_ctx_mutex / box64_set_err / box64_get_error (shared primitives)

mod cstring;
mod engine;
mod error;
mod ffi;
mod phase;
pub mod pt_mutex;

// Re-exported for the integration tests, which drive the phase machine through
// the Rust API (this also forces these `#[no_mangle]` leaves into the test
// link; a bare `extern "C"` declaration in the test is not enough to pull the
// rlib objects).
pub use engine::{
    box64dl_rust_phase_begin_close, box64dl_rust_phase_begin_init, box64dl_rust_phase_commit_close,
    box64dl_rust_phase_commit_init, box64dl_rust_phase_fail_init,
};

use std::ffi::{CStr, c_char};

use error::Global;

/// Non-zero only while the engine phase is READY.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn box64_is_ready() -> i32 {
    i32::from(engine::is_ready())
}

/// Diagnostic accessor: the phase as a stable integer (Phase as u8).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn box64_engine_phase() -> u8 {
    phase::current().as_u8()
}

/// The shared recursive context lock, handed to C so its entry points and this
/// crate serialise on the same instance.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn box64_ctx_mutex() -> *mut libc::pthread_mutex_t {
    phase::GLOBAL.raw()
}

/// Error-slot entry used by the C side. A NULL message clears the slot.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn box64_set_err(msg: *const c_char) {
    if msg.is_null() {
        Global::clear_err();
        return;
    }
    let text = unsafe { CStr::from_ptr(msg) }.to_string_lossy();
    Global::set_err(&text);
}

/// Error-slot entry used by the C side: read-once, NULL when empty or consumed.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn box64_get_error() -> *const c_char {
    Global::take_ptr()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn fresh_process_is_uninit_and_not_ready() {
        // No init in this test: the phase must be UNINIT and ready must be 0.
        assert_eq!(
            unsafe { box64_engine_phase() },
            phase::Phase::Uninit.as_u8()
        );
        assert_eq!(unsafe { box64_is_ready() }, 0);
    }

    #[test]
    fn set_and_get_error_roundtrip() {
        Global::set_err("boom");
        let p = unsafe { box64_get_error() };
        assert!(!p.is_null());
        assert_eq!(unsafe { CStr::from_ptr(p) }.to_str().unwrap(), "boom");
        // read consumes
        assert!(unsafe { box64_get_error() }.is_null());
        // NULL clears
        Global::set_err("again");
        unsafe { box64_set_err(std::ptr::null()) };
        assert!(unsafe { box64_get_error() }.is_null());
    }
}
