mod c_box64;
mod env;
mod log;
mod page_size;

use std::ffi::c_void;
use std::ptr::NonNull;
use std::sync::Mutex;

use anyhow::{bail, Result};
use c_box64::*;
use log::init_log;
use page_size::init_pagesize;

struct GlobalError {
    err: String,
}

impl GlobalError {
    pub const fn new() -> Self {
        Self { err: String::new() }
    }
    pub fn set_err(&mut self, msg: &str) {
        //我们在set_err时，认为前面的错误已经失效了，所以直接清空
        //与C不同
        self.clear_err();
        self.err.push_str(msg);
    }
    pub fn get_err(&self) -> &str {
        &self.err
    }
    pub fn clear_err(&mut self) {
        self.err.clear();
    }
}

struct Box64Context {
    //非零指针
    my_context: NonNull<c_box64_ctx>,
}

// NonNull 默认 !Send；我们把它放进 Mutex，跨线程移交指针本身没问题。
// 与 C 侧一致：全局 my_context + 大锁串行化 API（libbox64.c 的 g_lock）。
// Safety: 所有通过 Box64Lib 碰 context 的路径都持 BOX64_LIB 锁。
// 注意：C 内部仍会直接读全局 my_context，那部分不归这把锁管——和纯 C 嵌入时一样。
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


//C全局变量
//err:g_err
//ctx:my_context
//ctx(None):g_ready
struct Global {
    err: GlobalError,
    ctx: Option<Box64Context>,
}

static GLOBAL: Mutex<Global> = Mutex::new(Global {
    err: GlobalError::new(),
    ctx: None,
});

impl Global {
    /// 在已持有 GLOBAL 锁的前提下填充 g.lib。
    fn init(g: &mut Global) -> Result<()> {
        if g.ctx.is_some() {
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
        g.ctx = Some(context);
        Ok(())
    }
    fn is_ready(&self) -> bool {
        self.ctx.is_some()
    }
    fn context(&self) -> Option<&Box64Context> {
        self.ctx.as_ref()
    }
    fn require_context(&self) -> Result<&Box64Context> {
        self.ctx
            .as_ref()
            .ok_or_else(|| anyhow::anyhow!("context is not ready"))
    }
}

//线程不安全，每个线程独占
#[derive(Clone, Copy)]
struct X64Emu(NonNull<c_x64emu_t>);

impl X64Emu{
    pub fn new(global: &Global) -> Result<Self> {
        let mut stack_size:usize = 0;
        {
            let context = global.require_context()?;
            //获取栈大小
            assert!(context.stack_size() != 0);
            stack_size = context.stack_size();
        }
        
    }
    /// 当前线程的 emu;
    pub fn current() -> Option<Self> {
        //检查当前线程是否已经有一个emu
        if unsafe { c_thread_get_et() }.is_null() {
            return None;
        }
        NonNull::new(unsafe { c_thread_get_emu() }).map(Self)
    }
    /// 如果当前线程没有emu，则创建一个
    pub fn ensure_current(global: &Global) -> Result<Self> {
        //快速路径：检查emu是否已经存在
        if let Some(emu) = Self::current() {
            return Ok(emu);
        }
        //慢路径：创建emu
        X64Emu::new(global)?;
    }
    pub fn as_ptr(&self) -> *mut c_x64emu_t {
        self.0.as_ptr()
    }
}

pub fn lib_init() -> Result<()> {
    //初始化
    GLOBAL.lock()?.init()?;
    //创建当前线程的emu
    X64Emu::ensure_current()?;
    Ok()
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn box64_lib_init() -> i32 {
    match lib_init(){
        Ok(_) => 0,
        Err(e) => {
            log!("lib_init failed: {}", e);
            if let Ok(mut g) = GLOBAL.lock() {
                g.err.set_err(&e.to_string());
            }
            -1
        }
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn box64_is_ready() -> i32 {
    match GLOBAL.lock(){
        Ok(g) => i32::from(g.is_ready()),
        Err(_) => 0,
    }
}

#[unsafe(no_mangle)]
//question: 需要保证ensure_emu与C完全一致吗？(后者假设外部持锁)
//question: 需要改动rust的锁为重入锁吗？
pub unsafe extern "C" fn ensure_emu() -> i32 {
    let mut global = GLOBAL.lock()?;
    match X64Emu::ensure_current(&global) {
        Ok(_) => 0,
        Err(e) => {
            log!("ensure_emu failed: {}", e);
            global.err.set_err(&e.to_string());
            -1
        }
    }
}