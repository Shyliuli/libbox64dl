use anyhow::Context;
use libc::FILE;
use std::ffi::{c_void, CStr};
use std::ptr::NonNull;

/// 对应 C 的 box64context_t
#[repr(C)]
pub struct c_box64_ctx {
    _opaque: [u8; 0],
}
// 对应 C 的 x64emu_t
#[repr(C)]
pub struct c_x64emu_t{
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
    pub unsafe fn init_sysinfo();
    pub unsafe fn init_dynarec_host_check();
    #[link_name = "init_box64_trace"]
    pub unsafe fn c_init_box64_trace();
    #[link_name = "init_guest"]
    pub unsafe fn c_init_guest(context: *mut c_box64_ctx);
    #[link_name = "init_stack"]
    pub unsafe fn c_init_stack(context: *mut c_box64_ctx);
    #[link_name = "context_stack_size"]
    pub unsafe fn c_context_stack_size(context: *mut c_box64_ctx) -> usize;
    
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

    // threads.c — thread_get_et 只探测当前线程是否已有 et,绝不创建。
    #[link_name = "thread_get_et"]
    pub unsafe fn c_thread_get_et() -> *mut c_void; // emuthread_t*,仅用于判空

    // thread_get_emu 返回当前线程的emu,如果当前线程没有emu,则生成一个256k栈的emu
    #[link_name = "thread_get_emu"]
    pub unsafe fn c_thread_get_emu() -> *mut c_x64emu_t;

    pub static mut ftrace: *mut FILE;
    pub static mut stderr: *mut FILE;

    //box64全局页大小
    pub static mut box64_pagesize: usize;
    
    #[link_name = "new_x64emu"]
    pub unsafe fn c_new_x64emu(context: *mut c_box64_ctx) -> *mut c_x64emu_t;

    // src/signal_policy.c — 一次性发布信号策略（见 include/libbox64_signal.h，
    // patches/0005）。必须在任何 guest call 之前调用；`none` 在这里被拒绝。
    #[link_name = "box64dl_signal_policy_publish"]
    pub unsafe fn c_signal_policy_publish() -> i32;
    #[link_name = "box64dl_signal_policy_refusal"]
    pub unsafe fn c_signal_policy_refusal() -> *const libc::c_char;
}

pub fn init_box64_trace() {
    unsafe { c_init_box64_trace() };
}

/// 发布并冻结信号策略。`none`（不安装 box64 handler）被 C 侧拒绝：dynarec 的
/// protectDB/auto-SMC 与 guest fatal-signal/SEH 仿真都依赖这四个 handler。
/// 失败时把 C 侧给出的理由带回；此时尚未创建 context，也没有安装任何 handler。
pub fn signal_policy_publish() -> anyhow::Result<()> {
    if unsafe { c_signal_policy_publish() } != 0 {
        let reason = unsafe { CStr::from_ptr(c_signal_policy_refusal()) }
            .to_string_lossy()
            .into_owned();
        anyhow::bail!("{reason}");
    }
    Ok(())
}

pub fn new_x64emu(context: *mut c_box64_ctx) -> anyhow::Result<NonNull<c_x64emu_t>> {
    let emu = unsafe { c_new_x64emu(context) };
    //报错来自C函数；为空时（C 侧没来得及记录）退回通用消息
    NonNull::new(emu)
        .with_context(|| {
            match crate::Global::get_err() {
                Some(reason) => reason.to_string(),
                None => "new_x64emu failed".to_string(),
            }
        })
}
