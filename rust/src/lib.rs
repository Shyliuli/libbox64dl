//如果想要阅读这份源代码，建议到最后看对C暴露的接口部分回溯。
mod c_box64;
mod cstring;
mod env;
mod log;
mod page_size;
mod pt_mutex;
use pt_mutex::PtMutex;
use std::cell::RefCell;
use std::ffi::{c_void, CStr};
use std::ptr::NonNull;
use anyhow::{bail, Result};
use c_box64::*;
use cstring::CString;
use log::init_log;
use page_size::init_pagesize;

struct GlobalError {
    err: CString,
    //标记Error是否被读过
    fresh: bool,
}

impl GlobalError {
    pub fn new() -> Self {
        Self { err: CString::new(), fresh: false }
    }
    pub fn set_err(&mut self, msg: &str) {
        //我们在set_err时，认为前面的错误已经失效了，所以直接清空
        self.err.clear();
        //消息含 NUL 时在第一个 NUL 处截断，不视为失败
        let _ = self.err.push_str(msg);
        self.fresh = true;
    }
    /// 读即消费：连续第二次调用返回 None（对齐 POSIX dlerror 语义）
    pub fn get_err(&mut self) -> Option<CString> {
        if !self.fresh {
            return None;
        }
        self.fresh = false;
        Some(self.err.clone())
    }
    pub fn clear_err(&mut self) {
        self.fresh = false;
        self.err.clear();
    }
    /// 供 FFI 出口：直接给线程局部槽位的指针，避免克隆后悬空
    pub fn as_ptr(&self) -> *const libc::c_char {
        self.err.as_ptr()
    }
}

thread_local! {
    //每线程一个错误槽（glibc 同款语义）：谁调用的错谁读得到，无锁无竞争，
    //也不存在与 ctx 锁的锁序问题
    static ERR: RefCell<GlobalError> = RefCell::new(GlobalError::new());
}

struct Box64Context {
    //非零指针
    my_context: NonNull<c_box64_ctx>,
}

// Safety: 所有碰 context 的路径都被 GLOBAL.ctx
// 这把递归锁串行化。
unsafe impl Send for Box64Context {}

impl Box64Context {
    pub fn new() -> Result<Self> {
        //我们的库是用来跑动态链接库的
        //所以在调用Box64Context_new内部将argc=1传入
        let ptr = unsafe { Box64Context_new() };
        NonNull::new(ptr)
            .map(|my_context| Self { my_context })
            .ok_or_else(|| anyhow::anyhow!("Box64Context_new failed"))
    }

    pub fn as_ptr(&self) -> *mut c_box64_ctx {
        self.my_context.as_ptr()
    }
    /// 在node场景下，符号在.so/.node里;但是dlopen(NULL) 会返回host Node/Electron可执行文件
    /// 所以需要将box64lib重新绑定到包含 box64_lib_init 的模块
    pub fn rebind(&self) -> Result<()> {
        let ret = unsafe { c_rebind_box64lib(self.as_ptr(), box64_lib_init as *const c_void) };
        if ret != 0 {
            bail!("rebind_box64lib failed");
        }
        Ok(())
    }
    pub fn init_guest(&self) -> Result<()> {
        unsafe { c_init_guest(self.as_ptr()) };
        Ok(())
    }
    pub fn init_stack(&self) -> Result<()> {
        unsafe { c_init_stack(self.as_ptr()) };
        Ok(())
    }
    pub fn stack_size(&self) -> usize {
        unsafe { c_context_stack_size(self.as_ptr()) }
    }
}

impl Drop for Box64Context {
    fn drop(&mut self) {
        // C API 要 box64context_t**，会把 *context 置空
        let mut p = self.my_context.as_ptr();
        unsafe {
            FreeBox64Context(&mut p);
        }
    }
}
struct Global {
    //重入锁：C 共享这把锁
    ctx: PtMutex<Option<Box64Context>>,
}

static GLOBAL: Global = Global {
    ctx: PtMutex::new(None),
};

impl Global {
    fn set_err(msg: &str) {
        ERR.with_borrow_mut(|e| e.set_err(msg));
    }
    fn clear_err() {
        ERR.with_borrow_mut(|e| e.clear_err());
    }
    pub(crate) fn get_err() -> Option<CString> {
        ERR.with_borrow_mut(|e| e.get_err())
    }
    /// 惰性初始化 context。调用者必须已持有 ctx 锁（参数借自 guard）。
    fn init(ctx: &mut Option<Box64Context>) -> Result<()> {
        if ctx.is_some() {
            return Ok(());
        }

        init_pagesize();
        init_log();
        unsafe {
            init_sysinfo();
            init_dynarec_host_check();
        }

        //TODO: 在实现嵌入式singal处理，正确处理signal并转发给host/guest
        //Box64Context::new() 内部会调用 init_signal_helper()
        //这会导致host的handler直接挂掉
        //当前状态下的缺陷有：
        // 1.host缺页:收不到，被box64处理，可能错误处理，总之host会挂
        // 2.guest缺页:正确处理
        // 3.box64fault:正确处理
        //所以需要在init_signal_helper()中，保存host的handler并做合适的转发

        //新建模拟器上下文
        let context = Box64Context::new()?;

        // 重新绑定box64lib到正确的模块(而非host)
        context.rebind()?;
        // 初始化guest环境
        context.init_guest()?;
        unsafe {
            //加载当前进程的映射地址和权限到box64
            loadProtectionFromMap();
        }
        //初始化栈
        context.init_stack()?;

        //初始化box64 trace
        init_box64_trace();
        //设置全局变量
        *ctx = Some(context);
        Ok(())
    }
}

//线程不安全，每个线程独占
#[derive(Clone, Copy)]
struct X64Emu(NonNull<c_x64emu_t>);

impl X64Emu{
    /// 创建一个新的emu。
    pub fn new(context: *mut c_box64_ctx) -> Result<Self> {
        // 调用 C函数包装，这里需要调用box64的InternalMmap等，rust写比较麻烦
        let emu = new_x64emu(context)?;
        Ok(Self(emu))
    }
    /// 当前线程的 emu;
    pub fn current() -> Option<Self> {
        //检查当前线程是否已经有一个emu
        //注意，这里不应该使用c_thread_get_emu，这一函数会在没有emu时静默创建一个256k栈的emu！！
        if unsafe { c_thread_get_et() }.is_null() {
            return None;
        }
        //返回当前线程的emu
        NonNull::new(unsafe { c_thread_get_emu() }).map(Self)
    }
    /// 如果当前线程没有emu，则创建一个；调用者应持有 ctx 锁
    pub fn ensure_current(context: *mut c_box64_ctx) -> Result<Self> {
        //快速路径：检查emu是否已经存在
        if let Some(emu) = Self::current() {
            return Ok(emu);
        }
        //慢路径：创建emu
        Self::new(context)
    }
    pub fn as_ptr(&self) -> *mut c_x64emu_t {
        self.0.as_ptr()
    }
}

pub fn lib_init() -> Result<()> {
    let mut ctx = GLOBAL.ctx.lock();
    Global::init(&mut ctx)?;
    //获取context指针
    let context = ctx.as_ref().expect("init 失败！").as_ptr();
    //创建emu(emu需要context栈大小信息)
    X64Emu::ensure_current(context)?;
    Ok(())
}
//全局初始化
#[unsafe(no_mangle)]
pub unsafe extern "C" fn box64_lib_init() -> i32 {
    match lib_init(){
        Ok(_) => 0,
        Err(e) => {
            log!("lib_init failed: {}", e);
            Global::set_err(&e.to_string());
            -1
        }
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn box64_is_ready() -> i32 {
    i32::from(GLOBAL.ctx.lock().is_some())
}

///错误记录入口
#[unsafe(no_mangle)]
pub unsafe extern "C" fn box64_set_err(msg: *const libc::c_char) {
    if msg.is_null() {
        //清空错误
        Global::clear_err();
        return;
    }
    let msg = unsafe { CStr::from_ptr(msg) }.to_string_lossy();
    Global::set_err(&msg);
}

/// 确保当前线程有一个规格化栈的 emu。
/// 两种调用状态都要支持：C 入口持锁调用（递归锁重入），以及 uninode wrapped
/// 回调这类完全不持锁的路径（自取锁）。成功 0，失败 -1 并记录错误。
#[unsafe(no_mangle)]
pub unsafe extern "C" fn box64_ensure_emu() -> i32 {
    let ctx = GLOBAL.ctx.lock();
    //没有初始化context（对应旧 C 版的 my_context == NULL）
    let Some(context) = ctx.as_ref() else {
        Global::set_err("context is not ready");
        return -1;
    };
    let context = context.as_ptr();
    match X64Emu::ensure_current(context) {
        Ok(_) => 0,
        Err(e) => {
            log!("ensure_emu failed: {:?}", e);
            Global::set_err(&format!("ensure_emu failed: {:?}", e));
            -1
        }
    }
}
/// 供 libbox64.c 的 lock()/unlock()：给出共享递归锁的 pthread 句柄，
/// C 侧配 pt_mutex.h 的 pt_mutex_lock/pt_mutex_unlock 使用。
#[unsafe(no_mangle)]
pub unsafe extern "C" fn box64_ctx_mutex() -> *mut libc::pthread_mutex_t {
    GLOBAL.ctx.raw()
}

/// 线程局部错误槽出口，供 C 的 box64_dlerror：读到即消费；
/// 无错或已被消费返回 NULL。指针指向槽位本体，合法到本线程下一次错误操作
/// （set/clear 可能触发扩容搬移），即 glibc 式"活到下一次调用"契约。
#[unsafe(no_mangle)]
pub unsafe extern "C" fn box64_get_error() -> *const libc::c_char {
    ERR.with_borrow_mut(|e| match e.get_err() {
        Some(_) => e.as_ptr(),
        None => std::ptr::null(),
    })
}
