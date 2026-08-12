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
    pub fn new() -> Self {
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
struct Box64Lib {
    err: GlobalError,
    context: Box64Context,
}

static BOX64_LIB: Mutex<Option<Box64Lib>> = Mutex::new(None);

//question:能否把这些接受context的函数，移动到Box64Context的impl中？
impl Box64Lib {
    /// 在已持有 BOX64_LIB 锁的前提下，把 slot 从 None 填成 Some。
    fn init(slot: &mut Option<Self>) -> Result<()> {
        if slot.is_some() {
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

        // 在node场景下，符号在.so/.node里;但是dlopen(NULL) 会返回host Node/Electron可执行文件
        // 所以需要将box64lib重新绑定到包含 box64_lib_init 的模块
        rebind_box64lib(context.as_ptr(), box64_lib_init as *const c_void)?;

        unsafe {
            //初始化guest环境
            init_guest(context.as_ptr());
            //加载当前进程的映射地址和权限到box64
            loadProtectionFromMap();
        }
        //初始化栈
        init_stack(context.as_ptr())?;
        //准备emu实例(有防重入)
        ensure_emu()?;
        //初始化box64 trace
        init_box64_trace();

        //设置全局变量
        *slot = Some(Self {
            err: GlobalError::new(),
            context,
        });
        Ok(())
    }
}

// 临时：先调 C 的！以后改成 Rust 实现并删掉此声明。
// 走 C 侧导出的 box64_ensure_emu，避免依赖 static 符号。
unsafe extern "C" {
    #[link_name = "box64_ensure_emu"]
    fn c_ensure_emu() -> i32;
}

/// init 走这个包装；内部暂时转调 C。
fn ensure_emu() -> Result<()> {
    if unsafe { c_ensure_emu() } != 0 {
        bail!("Failed to initialize emulator");
    }
    Ok(())
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn box64_lib_init() -> i32 {
    let mut guard = match BOX64_LIB.lock() {
        Ok(guard) => guard,
        Err(e) => {
            log!("Failed to lock BOX64_LIB: {}", e);
            return -1;
        }
    };

    match Box64Lib::init(&mut guard) {
        Ok(()) => 0,
        Err(err) => {
            // slot 仍是 None，没有 GlobalError 字段可写
            log!("Failed to initialize box64: {:?}", err);
            -1
        }
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn box64_is_ready() -> i32 {
    match BOX64_LIB.lock() {
        Ok(guard) => i32::from(guard.is_some()),
        Err(_) => 0,
    }
}
