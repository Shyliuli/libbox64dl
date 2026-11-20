use abi_plan::{AbiPlanError, Plan, Register};
use cinline::*;
use std::ffi::*;
use std::sync::OnceLock;
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
        if NewBox64Context(1).is_null() {
            return -1;
        }
        c! {
            box64context_t* ctx = my_context;
            ctx->argv[0] = "box64dl";
            ctx->envv = environ;
            for (ctx->envc = 0; environ[ctx->envc]; ++ctx->envc) {}
        };
        InitializeEnvFiles();
        InitializeSystemInfo();
        0
    }
}
#[unsafe(no_mangle)]
unsafe extern "C" fn box64dl_init() {
    static INIT_RESULT: OnceLock<i32> = OnceLock::new();
    if *INIT_RESULT.get_or_init(global_init) == 0 {
        // todo: 做真正的线程初始化
        unsafe { thread_get_emu() };
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
        let plan: &Plan = &plan.as_ref().unwrap();
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
