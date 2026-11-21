use abi_plan::{AbiPlanError, Plan, Register};
use cinline::*;
use std::ffi::*;
use std::sync::OnceLock;
use anyhow::*;

#[allow(dead_code, non_camel_case_types, non_upper_case_globals, non_snake_case)]
mod bindings {
    include!(concat!(env!("OUT_DIR"), "/box64_bindings.rs"));
}
use bindings::*;
cinclude! {
    #include "box64dl_cinclude.h"
}
cmod!{
void box64_set_r8(x64emu_t* emu, uint64_t value)
{
    R_R8 = value;
}
void box64_set_r9(x64emu_t* emu, uint64_t value)
{
    R_R9 = value;
}
}

fn thread_init() -> Result<()> {
    // 已有emu
    if !unsafe { thread_get_emu_no_create() }.is_null() {
        return Ok(());
    }

    let status:i32= unsafe{c! { -> i32
        const int stack_size = 2 * 1024 * 1024;
        const int prot = PROT_READ | PROT_WRITE;
        void *stack = InternalMmap(NULL, stack_size, prot,
            MAP_PRIVATE | MAP_ANONYMOUS | MAP_GROWSDOWN, -1, 0);
        if (stack == MAP_FAILED) {
            return -1;
        }

        setProtection_stack((uintptr_t)stack, stack_size, prot);
        x64emu_t *emu = NewX64Emu(
            my_context, my_context->exit_bridge,
            (uintptr_t)stack, stack_size, 1);
        refreshTLSData(emu);
        thread_set_emu(emu);
        return 0;
    }};

    if status != 0 {
        bail!("failed to allocate the Box64 guest stack");
    }
    Ok(())
} 
fn init()->Result<()>{
    static INIT_RESULT: OnceLock<i32> = OnceLock::new();
    if *INIT_RESULT.get_or_init(global_init) == 0 {
        thread_init()?;
        Ok(())
    }
    else{
        bail!("box64dl init failed")
    }
}
fn global_init() -> i32{
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
        let mut ctx:*mut box64context_t=std::ptr::null_mut();
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
unsafe extern "C" fn box64dl_init()->i32 {
    match init(){
        Result::Ok(_) => {
            0
        },
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

#[unsafe(no_mangle)]
unsafe extern "C" fn box64dl_call(
    handle: *mut c_void,
    plan: *const Plan,
    args: *const u64,
    ret: *mut c_void,
) -> AbiPlanError {
    if handle.is_null() || plan.is_null() {
        return AbiPlanError::InvalidArgument;
    }

    unsafe {
        let plan: &Plan = plan.as_ref().unwrap();
        let write_result = match plan.result {
            Register::None => false,
            Register::Rax => true,
            _ => return AbiPlanError::NotSupport,
        };

        let emu = thread_get_emu();
        if emu.is_null() {
            return AbiPlanError::InvalidArgument;
        }

        for (i, arg) in plan.args.iter().enumerate() {
            let value = match arg {
                Register::None => break,
                _ => *args.add(i),
            };
            match arg {
                Register::Rdi => SetRDI(emu, value),
                Register::Rsi => SetRSI(emu, value),
                Register::Rdx => SetRDX(emu, value),
                Register::Rcx => SetRCX(emu, value),
                Register::R8 => box64_set_r8(emu, value),
                Register::R9 => box64_set_r9(emu, value),
                Register::Rax => SetRAX(emu, value),
                /*None 已经退出了，其他的报错*/
                _ => return AbiPlanError::NotSupport,
            }
        }

        DynaCall(emu, handle as usize, 0);
        if write_result && !ret.is_null() {
            // TODO: plan加入一个result_size字段，用于判断返回值类型
            // 现在abi_plan只支持int所以写入i32
            *ret.cast::<i32>() = GetRAX(emu) as i32;
        }
    }
    AbiPlanError::Ok
}
