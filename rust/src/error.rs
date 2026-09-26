//! Thread-local error slot (POSIX `dlerror` semantics: read consumes).
//!
//! One slot per thread, no lock, no contention. The pointer handed out by
//! `take_ptr()` points at the slot body and stays valid until this thread's next
//! error mutation (a consumed no-op clear does not touch the buffer).

use std::cell::RefCell;

use crate::cstring::CString;

pub struct GlobalError {
    err: CString,
    fresh: bool,
}

impl GlobalError {
    pub fn new() -> Self {
        Self {
            err: CString::new(),
            fresh: false,
        }
    }

    pub fn set_err(&mut self, msg: &str) {
        // A new error invalidates the previous one; NULs truncate, not fail.
        self.err.clear();
        let _ = self.err.push_str(msg);
        self.fresh = true;
    }

    #[allow(dead_code)]
    pub fn get_err(&mut self) -> Option<CString> {
        if !self.fresh {
            return None;
        }
        self.fresh = false;
        Some(self.err.clone())
    }

    pub fn take_ptr(&mut self) -> *const libc::c_char {
        if !self.fresh {
            return std::ptr::null();
        }
        self.fresh = false;
        self.err.as_ptr()
    }

    pub fn clear_err(&mut self) {
        if !self.fresh {
            return;
        }
        self.fresh = false;
        self.err.clear();
    }
}

impl Default for GlobalError {
    fn default() -> Self {
        Self::new()
    }
}

thread_local! {
    static ERR: RefCell<GlobalError> = RefCell::new(GlobalError::new());
}

/// Accessors used by the C shim and by the Rust entry points. The slot itself
/// lives in Rust only; C goes through the `box64_set_err`/`box64_get_error`
/// exports.
pub struct Global;

impl Global {
    pub fn set_err(msg: &str) {
        ERR.with_borrow_mut(|e| e.set_err(msg));
    }

    pub fn clear_err() {
        ERR.with_borrow_mut(|e| e.clear_err());
    }

    pub fn get_err() -> Option<CString> {
        ERR.with_borrow_mut(|e| e.get_err())
    }

    pub fn take_ptr() -> *const libc::c_char {
        ERR.with_borrow_mut(|e| e.take_ptr())
    }

    /// The last error as a String, if any (used for diagnostics).
    #[allow(dead_code)]
    pub fn last_error_string() -> Option<String> {
        Self::get_err().map(|c| c.to_string_lossy().into_owned())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn read_consumes() {
        Global::clear_err();
        Global::set_err("one");
        assert_eq!(Global::last_error_string().as_deref(), Some("one"));
        assert!(Global::last_error_string().is_none());
    }

    #[test]
    fn clear_then_set() {
        Global::set_err("old");
        Global::clear_err();
        assert!(Global::last_error_string().is_none());
        Global::set_err("new");
        assert_eq!(Global::last_error_string().as_deref(), Some("new"));
        Global::clear_err();
    }
}
