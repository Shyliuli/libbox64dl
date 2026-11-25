use abi_plan::{
    AbiPlanError, Allocation, CFunc, CType, CallPlan, Place, Register, free_plan, get_plan,
};
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
mod bindings {
    include!(concat!(env!("OUT_DIR"), "/box64_bindings.rs"));
}
use bindings::*;
cinclude! {
    #include "box64dl_cinclude.h"
}
cmod! {
void box64_set_r8(x64emu_t* emu, uint64_t value)
{
    R_R8 = value;
}
void box64_set_r9(x64emu_t* emu, uint64_t value)
{
    R_R9 = value;
}
void box64_set_xmm(x64emu_t* emu, int index, uint64_t value)
{
    emu->xmm[index].q[0] = value;
}
uint64_t box64_get_xmm(x64emu_t* emu, int index)
{
    return emu->xmm[index].q[0];
}
void box64_stack_push(x64emu_t* emu, size_t size)
{
    R_RSP -= size;
}
void box64_stack_pop(x64emu_t* emu, size_t size)
{
    R_RSP += size;
}
void box64_stack_write(x64emu_t* emu, size_t offset, const void* src, size_t size)
{
    memcpy((void*)(R_RSP + offset), src, size);
}
void box64_dynacall(x64emu_t* emu, void* fn)
{
    DynaCall(emu, (uintptr_t)fn, 0);
}
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

/// 从 `src` 按小端读取 `len` 字节，用于把参数值搬进模拟寄存器。
unsafe fn load(src: *const c_void, len: usize) -> u64 {
    match len {
        1 => (unsafe { src.cast::<u8>().read() }) as u64,
        2 => (unsafe { src.cast::<u16>().read_unaligned() }) as u64,
        4 => (unsafe { src.cast::<u32>().read_unaligned() }) as u64,
        _ => unsafe { src.cast::<u64>().read_unaligned() },
    }
}

/// 把 `value` 按小端写入 `dst` 的前 `len` 字节，用于写回返回值。
unsafe fn store(dst: *mut c_void, len: usize, value: u64) {
    match len {
        1 => unsafe { dst.cast::<u8>().write(value as u8) },
        2 => unsafe { dst.cast::<u16>().write_unaligned(value as u16) },
        4 => unsafe { dst.cast::<u32>().write_unaligned(value as u32) },
        _ => unsafe { dst.cast::<u64>().write_unaligned(value) },
    }
}

/// `Xmm0..Xmm7` 在 `x64emu_t.xmm` 里的下标。
fn xmm_index(reg: Register) -> Option<usize> {
    Some(match reg {
        Register::Xmm0 => 0,
        Register::Xmm1 => 1,
        Register::Xmm2 => 2,
        Register::Xmm3 => 3,
        Register::Xmm4 => 4,
        Register::Xmm5 => 5,
        Register::Xmm6 => 6,
        Register::Xmm7 => 7,
        _ => return None,
    })
}

/// 按 plan 给出的寄存器写入一个值；plan 里不会出现的寄存器返回 false。
unsafe fn set_reg(emu: *mut x64emu_t, reg: Register, value: u64) -> bool {
    // 浮点分支
    if let Some(index) = xmm_index(reg) {
        unsafe { box64_set_xmm(emu, index as i32, value) };
        return true;
    }
    // 整数分支，只处理传参的6个
    unsafe {
        match reg {
            Register::Rdi => SetRDI(emu, value),
            Register::Rsi => SetRSI(emu, value),
            Register::Rdx => SetRDX(emu, value),
            Register::Rcx => SetRCX(emu, value),
            Register::Rax => SetRAX(emu, value),
            Register::R8 => box64_set_r8(emu, value),
            Register::R9 => box64_set_r9(emu, value),
            _ => return false,
        }
    }
    true
}

/// 读回 plan 指定的寄存器；未支持的寄存器返回 `None`。
unsafe fn get_reg(emu: *mut x64emu_t, reg: Register) -> Option<u64> {
    //浮点分支
    if let Some(index) = xmm_index(reg) {
        return Some(unsafe { box64_get_xmm(emu, index as i32) });
    }
    //整数分支
    //TODO: 或许需要RDX来支持int128？
    match reg {
        Register::Rax => Some(unsafe { GetRAX(emu) }),
        _ => None,
    }
}

unsafe fn write_result(emu: *mut x64emu_t, ret: &Place, retval: *mut c_void) {
    let Allocation::Reg { reg, len } = ret.alloc else {
        return;
    };
    // SAFETY: Reg 数组由 plan 提供，len 为其长度。
    let regs = unsafe { std::slice::from_raw_parts(reg, len) };
    // 单个寄存器的返回值按 `ret.size` 写；两个寄存器（16 字节标量）各写 8 字节。
    let width = if regs.len() > 1 { 8 } else { ret.size };
    for (slot, register) in regs.iter().enumerate() {
        if let Some(value) = unsafe { get_reg(emu, *register) } {
            unsafe { store(retval.byte_add(slot * 8), width, value) };
        }
    }
}

/// 静态路径：按现成的 `CallPlan` 设置寄存器和栈参数，`DynaCall`，再写回返回值。
///
/// val:plan.args等长的数组，args的值
#[unsafe(no_mangle)]
unsafe extern "C" fn box64dl_static_call(
    handle: *mut c_void,
    plan: *const CallPlan,
    val: *const *const c_void,
    retval: *mut c_void,
) -> AbiPlanError {
    // 0. 参数检查
    if handle.is_null() || plan.is_null() {
        return AbiPlanError::InvalidArgument;
    }
    // SAFETY: `plan` 已检查非空。
    let plan = unsafe { plan.as_ref().unwrap() };
    let args = plan.args();
    let has_result = !matches!(plan.ret.alloc, Allocation::Void);
    if (!args.is_empty() && val.is_null()) || (has_result && retval.is_null()) {
        return AbiPlanError::InvalidArgument;
    }

    let emu = unsafe { thread_get_emu() };
    if emu.is_null() {
        return AbiPlanError::InvalidArgument;
    }

    // 1. 按 plan 预留栈参数空间；`stack_size` 已是 16 的倍数。
    let stack_size = plan.stack_size;
    if stack_size > 0 {
        unsafe { box64_stack_push(emu, stack_size) };
    }

    // 把 `val` 看成与 `args` 等长的指针数组；`args` 为空时 `val` 允许为 null。
    let vals = if args.is_empty() {
        &[][..]
    } else {
        // SAFETY: 上方已检查 `val` 非空；plan 保证其指向 `args.len()` 个有效参数地址。
        unsafe { std::slice::from_raw_parts(val, args.len()) }
    };

    // 2. 按参数顺序写寄存器和栈槽。
    // arg_place,arg_val_ptr
    for (place, &src) in args.iter().zip(vals) {
        match place.alloc {
            Allocation::Reg { reg, len } => {
                // 寄存器分支
                let regs = unsafe { std::slice::from_raw_parts(reg, len) };
                // 单寄存器则写size字节，双寄存器则各写8字节
                let width = if regs.len() > 1 { 8 } else { place.size };
                for (slot, register) in regs.iter().enumerate() {
                    // value = src[slot * 8]
                    let value = unsafe { load(src.byte_add(slot * 8), width) };
                    
                    if !unsafe { set_reg(emu, *register, value) } {
                        // 错误处理
                        if stack_size > 0 {
                            unsafe { box64_stack_pop(emu, stack_size) };
                        }
                        return AbiPlanError::Unsupported;
                    }
                }
            }
            Allocation::Stack { offset } => unsafe {
                box64_stack_write(emu, offset, src, place.size);
            },
            _ => {
                if stack_size > 0 {
                    unsafe { box64_stack_pop(emu, stack_size) };
                }
                return AbiPlanError::Unsupported;
            }
        }
    }

    // DynaCall 会把 RSP 恢复成进入时的值，调用后不需要再还栈。
    // TODO: 变参函数需要按 SysV 约定把 AL 设成使用的向量寄存器个数，当前未处理。
    unsafe { box64_dynacall(emu, handle) };

    // 3. 按 plan.ret 读回返回值。
    if has_result {
        unsafe { write_result(emu, &plan.ret, retval) };
    }
    AbiPlanError::Ok
}

/// 动态路径：`get_plan` 后转调 `box64dl_static_call`，再 `free_plan`。
///
/// # Safety
/// `args` 在 `args_len > 0` 时须指向 `args_len` 个有效 `CType`；
/// 其余要求同 `box64dl_static_call`。
#[unsafe(no_mangle)]
unsafe extern "C" fn box64dl_call(
    handle: *mut c_void,
    args: *const CType,
    args_len: u32,
    val: *const *const c_void,
    ret: CType,
    retval: *mut c_void,
) -> AbiPlanError {
    if args_len > 0 && args.is_null() {
        return AbiPlanError::InvalidArgument;
    }

    let cfunc = CFunc {
        args: args as *mut CType,
        args_len,
        ret,
    };
    // SAFETY:
    // CallPlan保持CABI，允许zeroed初始化,被get_plan覆盖
    // free_plan禁止多次释放
    unsafe {
        let mut p: CallPlan = std::mem::zeroed();
        // status=get_plan(cfunc, raw_plan)
        let err = get_plan(cfunc, &mut p);
        if err != AbiPlanError::Ok {
            return err;
        }
        let err = box64dl_static_call(handle, &p, val, retval);
        free_plan(p);
        err
    }
}
