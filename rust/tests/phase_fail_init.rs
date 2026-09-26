//! Integration test for the FAILED terminal phase: a failed initialization
//! cannot be retried, and closing a never-READY engine is a no-op.

use libbox64::{
    box64_engine_phase, box64dl_rust_phase_begin_close, box64dl_rust_phase_begin_init,
    box64dl_rust_phase_fail_init,
};

const INITIALIZING: u8 = 1;
const FAILED: u8 = 3;

#[test]
fn failed_init_is_terminal() {
    unsafe {
        assert_eq!(box64dl_rust_phase_begin_init(), 0);
        assert_eq!(box64_engine_phase(), INITIALIZING);

        box64dl_rust_phase_fail_init();
        assert_eq!(box64_engine_phase(), FAILED);

        // FAILED is terminal for init...
        assert_eq!(box64dl_rust_phase_begin_init(), -3);
        // ...and there is nothing to close.
        assert_eq!(box64dl_rust_phase_begin_close(), -1);
    }
}
