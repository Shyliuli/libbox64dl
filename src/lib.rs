use abi_plan::*;
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
    typedef struct Xmm { uint64_t lo; uint64_t hi; } Xmm;
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
#[derive(Clone, Copy)]
#[repr(C)]
pub struct Xmm {
    pub lo: u64,
    pub hi: u64,
}
fn align_down(x: u64, align: u64) -> u64 {
    x & !(align - 1)
}

fn is_xmm(r: Register) -> bool {
    (r as u32) >= (Register::Xmm0 as u32)
}

cmod! {
    void write_reg(x64emu_t* emu, uint32_t r, uint64_t v) {
        emu->regs[r].q[0] = v;
    }
    uint64_t read_reg(x64emu_t* emu, uint32_t r) {
        return emu->regs[r].q[0];
    }
    void write_xmm(x64emu_t* emu, uint32_t r, Xmm x) {
        uint32_t i = r - 10;
        emu->xmm[i].q[0] = x.lo;
        emu->xmm[i].q[1] = x.hi;
    }
    Xmm read_xmm(x64emu_t* emu, uint32_t r) {
        uint32_t i = r - 10;
        Xmm x = { emu->xmm[i].q[0], emu->xmm[i].q[1] };
        return x;
    }
}

unsafe extern "C" {
    fn write_reg(emu: *mut x64emu_t, r: Register, v: u64);
    fn read_reg(emu: *mut x64emu_t, r: Register) -> u64;
    fn write_xmm(emu: *mut x64emu_t, r: Register, x: Xmm);
    fn read_xmm(emu: *mut x64emu_t, r: Register) -> Xmm;
}

unsafe fn write_place(emu: *mut x64emu_t, sp: u64, place: &Place, src: *const u8) {
    unsafe {
        match place.alloc {
            Allocation::Void => {}
            Allocation::Stack { offset } => {
                // 直接拷贝
                std::ptr::copy_nonoverlapping(src, (sp as usize + offset) as *mut u8, place.size);
            }
            Allocation::Reg { regs: r, len } => {
                let mut v = [0u64; 2];
                std::ptr::copy_nonoverlapping(src, v.as_mut_ptr().cast(), place.size);
                for k in 0..len as usize {
                    if is_xmm(r[k]) {
                        // 暂不支持16字节全塞到一个xmm的类型
                        assert!(place.size <= 8 * len as usize);
                        write_xmm(emu, r[k], Xmm { lo: v[k], hi: 0 });
                    } else {
                        write_reg(emu, r[k], v[k]);
                    }
                }
            }
            Allocation::Sret => unreachable!("args layout cannot be sret"),
        }
    }
}

unsafe fn read_place(emu: *mut x64emu_t, place: &Place, dst: *mut u8) {
    unsafe {
        match place.alloc {
            Allocation::Void => {}
            Allocation::Reg { regs: r, len } => {
                assert!(place.size <= 16 && (len == 1 || len == 2));
                let mut v = [0u64; 2];
                for k in 0..len as usize {
                    if is_xmm(r[k]) {
                        assert!(place.size <= 8 * len as usize);
                        v[k] = read_xmm(emu, r[k]).lo;
                    } else {
                        v[k] = read_reg(emu, r[k]);
                    }
                }
                std::ptr::copy_nonoverlapping(v.as_ptr().cast(), dst, place.size);
            }
            Allocation::Sret => todo!("sret return"),
            Allocation::Stack { .. } => unreachable!("return layout cannot be stack"),
        }
    }
}

// args_val: void**，每项指向一个参数值；ret_val: 指向返回值缓冲区
fn call_plan(func: *mut c_void, plan: &CallPlan, args_val: *mut c_void, ret_val: *mut c_void) -> i32 {
    let emu = unsafe { thread_get_emu() };
    if emu.is_null() || func.is_null() {
        return -1;
    }
    let args = if plan.args_len == 0 {
        &[][..]
    } else {
        unsafe { core::slice::from_raw_parts(plan.args, plan.args_len) }
    };
    let vals = args_val as *const *const u8;

    unsafe {
        let old_sp = read_reg(emu, Register::Rsp);
        // 栈向下增长，16字节对齐
        let sp = align_down(old_sp - plan.stack_size as u64, 16);
        write_reg(emu, Register::Rsp, sp);

        for (i, arg) in args.iter().enumerate() {
            write_place(emu, sp, arg, *vals.add(i));
        }
        DynaCall(emu, func as usize, 0);
        write_reg(emu, Register::Rsp, old_sp);

        read_place(emu, &plan.ret, ret_val.cast());
    }
    0
}
// 运行时调用，性能较差
// 使用类似ffi的接口
#[unsafe(no_mangle)]
unsafe extern "C" fn box64dl_call_runtime(
    func: *mut c_void,
    args: CFunc,
    args_val: *mut c_void,
    ret_val: *mut c_void,
) -> i32 {
    let mut plan:CallPlan=unsafe{std::mem::zeroed()};
    let status=abi_plan_gen(args,&mut plan);
    if status!=0{
        return status;
    }
    // FIXME: 调用完需释放 plan.args (free_plan)，且高频调用应将 plan 预生成与执行拆开
    call_plan(func,&plan,args_val,ret_val)
}
// JIT路径看起来会非常美妙，但是要等abi-plan全做好了....
//
// TODO: 实现 trait 化，使 call_plan 流程可复用于三个后端：
//   - Exec: 直接执行寄存器/栈操作
//   - CCodegen: 生成 C 桩代码
//   - Emit: 生成内存中的 host 机器码
//
// TODO: CCodegen 后端 (AOT 路径)
//   输入 C 签名，编译时跑 X64Plan，输出操作 box64 状态的 C 桩函数，交给 C 编译器与主程序一起编译。
//
// TODO: Emit 后端 (JIT 路径，待 abi-plan 完成 LoongArch 等 HostPlan 后实现)
//   以 CFunc 或类型字符串为哈希查表，在内存中生成桩函数的 host 机器码：
//   1. 用 HostPlan 生成从宿主函数调用中取得参数的机器码（CCodegen 中这一步由 host C 编译器完成，
//      运行时无编译器参与故需 HostPlan；）；
//   2. 用 X64Plan 生成与 CCodegen 一致的修改 box64 寄存器/栈状态的机器码；
//   3. 调用 DynaCall，再按 X64Plan -> HostPlan 取回返回值。
#[unsafe(no_mangle)]
unsafe extern "C" fn box64dl_call(func: *mut c_void) -> i32 {
    unsafe {
        let emu = thread_get_emu();
        if emu.is_null() || func.is_null() {
            return -1;
        }
        DynaCall(emu, func as usize, 0);
        0
    }
}

// 下面三个函数用于代码生成时直接操作状态

// 获取通用寄存器堆 (reg64_t regs[16])
#[unsafe(no_mangle)]
unsafe extern "C" fn box64dl_get_thread_reg() -> *mut u64 {
    unsafe {
        let emu = thread_get_emu();
        if emu.is_null() {
            return std::ptr::null_mut();
        }
        let p = c! { (emu: *mut x64emu_t) -> *mut u64
            return (u64*)emu->regs;
        };
        p
    }
}

// 获取xmm寄存器堆 (sse_regs_t xmm[16])
#[unsafe(no_mangle)]
unsafe extern "C" fn box64dl_get_thread_reg_xmm() -> *mut Xmm {
    unsafe {
        let emu = thread_get_emu();
        if emu.is_null() {
            return std::ptr::null_mut();
        }
        let p = c! { (emu: *mut x64emu_t) -> usize
            return (uintptr_t)emu->xmm;
        };
        p as *mut Xmm
    }
}

// 获取栈基址 (emu->init_stack；压栈直接用 regs[_SP] 即可)
#[unsafe(no_mangle)]
unsafe extern "C" fn box64dl_get_thread_stack() -> *mut c_void {
    unsafe {
        let emu = thread_get_emu();
        if emu.is_null() {
            return std::ptr::null_mut();
        }
        let p = c! { (emu: *mut x64emu_t) -> usize
            return (uintptr_t)emu->init_stack;
        };
        p as *mut c_void
    }
}