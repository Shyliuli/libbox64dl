use anyhow::{bail, Result};
use libc::FILE;
use std::ffi::c_void;

/// 对应 C 的 box64context_t
#[repr(C)]
pub struct c_box64_ctx {
    _opaque: [u8; 0],
}

unsafe extern "C" {
    pub unsafe fn LoadEnvVariables();
    pub unsafe fn InitializeEnvFiles();
    
    // box64/src/core.c — patches/0001-export-openFTrace.patch
    // patch:导出这一c函数，用于在rust中调用
    pub unsafe fn openFTrace();
    pub unsafe fn PrintfFtrace(prefix: i32, fmt: *const libc::c_char, ...);
    
    pub unsafe fn InitializeSystemInfo();
    pub unsafe fn init_dynarec_host_check();
    #[link_name = "init_box64_trace"]
    pub unsafe fn c_init_box64_trace();
    pub unsafe fn init_guest(context: *mut c_box64_ctx);

    pub unsafe fn Box64Context_new() -> *mut c_box64_ctx;
    pub unsafe fn FreeBox64Context(context: *mut *mut c_box64_ctx);
    
    pub unsafe fn loadProtectionFromMap();
    
    #[link_name = "rebind_box64lib"]
    pub unsafe fn c_rebind_box64lib(
        my_context: *mut c_box64_ctx,
        box64_fn: *const c_void,
    ) -> i32;
    
    #[link_name = "CalcStackSize"]
    pub unsafe fn c_CalcStackSize(context: *mut c_box64_ctx) -> i32;

    pub static mut ftrace: *mut FILE;
    pub static mut stderr: *mut FILE;

    //box64全局页大小
    pub static mut box64_pagesize: usize;
}

pub fn rebind_box64lib(my_context: *mut c_box64_ctx, box64_fn: *const c_void) -> Result<()> {
    // Safety: my_context 来自 NewBox64Context；box64_fn 为本 so 内函数地址
    let ret = unsafe { c_rebind_box64lib(my_context, box64_fn) };
    if ret == 0 {
        Ok(())
    } else {
        bail!("Failed to rebind box64lib");
    }
}
pub fn init_stack(my_context: *mut c_box64_ctx) -> Result<()> {
    let ret = unsafe { c_CalcStackSize(my_context) };
    if ret == 0 {
        Ok(())
    } else {
        bail!("Failed to initialize stack");
    }
}
pub fn init_box64_trace() {
    unsafe { c_init_box64_trace() };
}