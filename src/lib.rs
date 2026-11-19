use abi_plan::{AbiPlanError, CType, Plan, Register, get_plan};
use std::ffi::*;
/*这里之后要进行很多包装...现在先跑起来*/
#[allow(non_snake_case)]
unsafe extern "C" {
    fn global_init() -> c_int;
    fn thread_init() -> c_int;

    fn thread_get_emu() -> *mut c_void;
    fn my_dlopen(emu: *mut c_void, filename: *const c_char, flag: c_int) -> *mut c_void;
    fn my_dlsym(emu: *mut c_void, handle: *mut c_void, symbol: *const c_char) -> *mut c_void;
    fn my_dlclose(emu: *mut c_void, handle: *mut c_void) -> c_int;

    fn SetRDI(emu: *mut c_void, value: u64);
    fn SetRSI(emu: *mut c_void, value: u64);
    fn SetRDX(emu: *mut c_void, value: u64);
    fn SetRCX(emu: *mut c_void, value: u64);
    fn SetRAX(emu: *mut c_void, value: u64);
    fn box64_set_r8(emu: *mut c_void, value: u64);
    fn box64_set_r9(emu: *mut c_void, value: u64);
    fn GetRAX(emu: *mut c_void) -> u64;
    fn DynaCall(emu: *mut c_void, addr: usize, no_alt: c_int);
}

#[unsafe(no_mangle)]
unsafe extern "C" fn box64dl_get_plan(
    args: *const CType,
    len: c_int,
    result: CType,
    plan: *mut Plan,
) -> AbiPlanError {
    unsafe { get_plan(args, len, result, plan) }
}

#[unsafe(no_mangle)]
unsafe extern "C" fn box64dl_init() {
    /*box64的init超级长，并且很多是针对“运行一个程序”的*/
    unsafe {
        global_init();
        thread_init();
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
        let plan:&Plan = &plan.as_ref().unwrap();
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
                _ => {
                    *args.add(i)
                }
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
                _=> return AbiPlanError::NotSupport,
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
