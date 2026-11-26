use anyhow::*;
use cinline::*;
use std::ffi::*;
use std::sync::OnceLock;

#[allow(
    dead_code,
    non_camel_case_types,
    non_upper_case_globals,
    non_snake_case
)]
#[path = "box64_bindings.rs"]
mod bindings;
use bindings::*;
cinclude! {
    #include "box64dl_cinclude.h"
}

fn thread_init() -> Result<()> {
    if !unsafe { thread_get_emu_no_create() }.is_null() {
        return Ok(());
    }

    const STACK_SIZE: usize = 2 * 1024 * 1024;
    let mut stack: usize = 0;
    let mut emu: *mut x64emu_t = std::ptr::null_mut();

    let status: i32 = unsafe {
        c! {
            (STACK_SIZE:usize, stack:usize, emu:*mut x64emu_t) -> i32
            const int prot = PROT_READ | PROT_WRITE;

            stack = (uintptr_t)InternalMmap(NULL, STACK_SIZE, prot,
                MAP_PRIVATE | MAP_ANONYMOUS | MAP_GROWSDOWN, -1, 0);
            if (stack == (uintptr_t)MAP_FAILED) return -1;

            setProtection_stack(stack, STACK_SIZE, prot);

            emu = NewX64Emu(my_context, my_context->exit_bridge,
                            (uintptr_t)stack, STACK_SIZE, 1);
            if (emu == NULL) {
                InternalMunmap((void*)stack, STACK_SIZE);
                return -2;
            }
            return 0;
        }
    };

    match status {
        0 => {}
        -1 => bail!("failed to allocate guest stack (mmap)"),
        -2 => bail!("NewX64Emu returned NULL"),
        n => bail!("thread_init failed with unknown status {n}"),
    }

    unsafe {
        refreshTLSData(emu);
        thread_set_emu(emu);
    }
    Ok(())
}
fn init() -> Result<()> {
    static INIT_RESULT: OnceLock<i32> = OnceLock::new();
    if *INIT_RESULT.get_or_init(global_init) == 0 {
        thread_init()?;
        Ok(())
    } else {
        bail!("box64dl init failed")
    }
}
fn global_init() -> i32 {
    unsafe {
        init_guestalloc();
        if init_auxval(0, std::ptr::null_mut(), environ) != 0 {
            return -1;
        }
        c! {
            long pagesize = sysconf(_SC_PAGESIZE);
            box64_pagesize = pagesize > 0 ? pagesize : 4096;
        };
        LoadEnvVariables();
        //在这个函数内部已经调用了InitAllHelpers
        if NewBox64Context(1).is_null() {
            return -1;
        }
        let mut ctx: *mut box64context_t = std::ptr::null_mut();
        c! {(ctx:*mut box64context_t)
            ctx = my_context;
            ctx->argv[0] = "box64dl";
            ctx->envv = environ;
            for (ctx->envc = 0; environ[ctx->envc]; ++ctx->envc) {}
        };
        addLibPaths(ctx);
        LoadLDPath(ctx);
        InitializeEnvFiles();
        InitializeSystemInfo();
        0
    }
}
#[unsafe(no_mangle)]
unsafe extern "C" fn box64dl_init() -> i32 {
    match init() {
        Result::Ok(_) => 0,
        Err(e) => {
            eprintln!("{e}");
            -1
        }
    }
}

#[unsafe(no_mangle)]
unsafe extern "C" fn box64dl_dlopen(path: *const c_char, flags: c_int) -> *mut c_void {
    unsafe {
        let emu = thread_get_emu();
        if emu.is_null() {
            return std::ptr::null_mut();
        }
        my_dlopen(emu, path, flags)
    }
}

#[unsafe(no_mangle)]
unsafe extern "C" fn box64dl_sym(handle: *mut c_void, name: *const c_char) -> *mut c_void {
    unsafe {
        let emu = thread_get_emu();
        if emu.is_null() {
            return std::ptr::null_mut();
        }
        my_dlsym(emu, handle, name)
    }
}

#[unsafe(no_mangle)]
unsafe extern "C" fn box64dl_close(handle: *mut c_void) -> c_int {
    unsafe {
        let emu = thread_get_emu();
        if emu.is_null() {
            return -1;
        }
        my_dlclose(emu, handle)
    }
}
