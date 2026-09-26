//! Integration test for the Rust-owned phase machine: the READY -> CLOSING ->
//! FINAL sequence and each refusal code.
//!
//! This runs in its own process (integration test binary), so the process-wide
//! phase starts UNINIT and the `lib.rs` unit tests are unaffected.

use core::ffi::c_void;

use libbox64::{
    box64_engine_phase, box64_is_ready, box64dl_rust_phase_begin_close,
    box64dl_rust_phase_begin_init, box64dl_rust_phase_commit_close, box64dl_rust_phase_commit_init,
};

const UNINIT: u8 = 0;
const INITIALIZING: u8 = 1;
const READY: u8 = 2;
const CLOSING: u8 = 4;
const FINAL: u8 = 5;

#[test]
fn ready_closing_final_sequence_and_refusals() {
    unsafe {
        assert_eq!(box64_engine_phase(), UNINIT);
        assert_eq!(box64_is_ready(), 0);

        // UNINIT -> INITIALIZING, and a concurrent init is refused while the
        // first owns the transition.
        assert_eq!(box64dl_rust_phase_begin_init(), 0);
        assert_eq!(box64_engine_phase(), INITIALIZING);
        assert_eq!(box64_is_ready(), 0);
        assert_eq!(box64dl_rust_phase_begin_init(), -2);

        // INITIALIZING -> READY. The context pointer is an identity handle
        // only, so a non-null sentinel is enough for this unit-level test.
        assert_eq!(
            box64dl_rust_phase_commit_init(0x1000usize as *mut c_void),
            0
        );
        assert_eq!(box64_engine_phase(), READY);
        assert_eq!(box64_is_ready(), 1);

        // Idempotent while READY.
        assert_eq!(box64dl_rust_phase_begin_init(), 1);

        // READY -> CLOSING publishes the refusal phase before any teardown.
        assert_eq!(box64dl_rust_phase_begin_close(), 0);
        assert_eq!(box64_engine_phase(), CLOSING);
        assert_eq!(box64_is_ready(), 0);
        // A second close cannot own the teardown.
        assert_eq!(box64dl_rust_phase_begin_close(), -1);

        // CLOSING -> FINAL is terminal.
        box64dl_rust_phase_commit_close();
        assert_eq!(box64_engine_phase(), FINAL);
        assert_eq!(box64dl_rust_phase_begin_init(), -4);
    }
}
