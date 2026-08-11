use crate::c_box64;
use crate::env;

pub fn ftrace_print(msg: &str) {
    let c = std::ffi::CString::new(msg).unwrap_or_default();
    unsafe {
        c_box64::PrintfFtrace(1, c"%s\n".as_ptr(), c.as_ptr());
    }
}

macro_rules! log {
    ($($arg:tt)*) => {
        $crate::log::ftrace_print(&format!($($arg)*))
    };
}
pub use log;

pub fn init_log() {
    env::init_env();
    unsafe {
        //默认值，openFTrace在对应env未设置时，不赋值
        c_box64::ftrace = c_box64::stderr;
        c_box64::openFTrace();
    }
}
