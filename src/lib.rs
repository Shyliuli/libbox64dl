use abi_plan::*;
use anyhow::*;
use cinline::*;
use std::ffi::*;
use std::fmt::Write;
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
            setenv("BOX64_NOBANNER", "1", 0);
            setenv("BOX64_LOG", "0", 0);
            setenv("BOX64_NORCFILES", "1", 0);
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
    fn malloc(size: usize) -> *mut c_void;
    fn free(ptr: *mut c_void);
}
// 我们将Call抽象为一个解释器
// 它读取CallPlan，并解析执行一些操作
// 一个直接的想法是，如果它通过这些标准接口操作
// 并允许我么将标准接口替换为生成等价C代码或者生成机器码
// 那么我们将自动获得一个代码生成器和编译器(后者可以用于jit)
// 对于编译器的目的，我们也可以将函数标记为always_inline
// 并将CallPlan在编译期生成为常量
// 在足够的优化等级下，它会一路常量展开，去循环.....
// 变成一个“被编译出来的最优的call”，效果和生成C代码再编译几乎等价
// 但这听起来并不是很好维护...
trait CallBackend {
    fn alloc_stack(&mut self, size: u64);
    fn restore_stack(&mut self);
    fn setup_sret(&mut self);
    fn arg_to_stack(&mut self, idx: usize, offset: usize, size: usize);
    fn arg_to_reg(&mut self, idx: usize, reg: Register, byte_off: usize, size: usize);
    fn call(&mut self);
    fn ret_from_reg(&mut self, reg: Register, byte_off: usize, size: usize);
}
// 抽象出来的call解释器
fn lower_call<B: CallBackend>(plan: &CallPlan, args: &[Place], b: &mut B) {
    b.alloc_stack(plan.stack_size as u64);
    if matches!(plan.ret.alloc, Allocation::Sret) {
        b.setup_sret();
    }
    for (i, p) in args.iter().enumerate() {
        match p.alloc {
            Allocation::Void => {}
            Allocation::Stack { offset } => b.arg_to_stack(i, offset, p.size),
            Allocation::Reg { regs, len } => {
                assert!(p.size <= 8 * len as usize);
                for k in 0..len as usize {
                    b.arg_to_reg(i, regs[k], k * 8, (p.size - k * 8).min(8));
                }
            }
            Allocation::Sret => unreachable!("args layout cannot be sret"),
        }
    }
    b.call();
    b.restore_stack();
    match plan.ret.alloc {
        Allocation::Void | Allocation::Sret => {}
        Allocation::Reg { regs, len } => {
            assert!(plan.ret.size <= 8 * len as usize);
            for k in 0..len as usize {
                b.ret_from_reg(regs[k], k * 8, (plan.ret.size - k * 8).min(8));
            }
        }
        Allocation::Stack { .. } => unreachable!("return layout cannot be stack"),
    }
}
// 后端:直接执行，作为解释器
struct ExecBackend {
    emu: *mut x64emu_t,
    func: usize,
    vals: *const *const u8,
    ret_val: *mut u8,
    old_sp: u64,
    sp: u64,
}

impl CallBackend for ExecBackend {
    fn alloc_stack(&mut self, size: u64) {
        self.old_sp = unsafe { read_reg(self.emu, Register::Rsp) };
        self.sp = align_down(self.old_sp - size, 16);
        unsafe { write_reg(self.emu, Register::Rsp, self.sp) };
    }
    fn restore_stack(&mut self) {
        unsafe { write_reg(self.emu, Register::Rsp, self.old_sp) };
    }
    fn setup_sret(&mut self) {
        assert!(!self.ret_val.is_null());
        unsafe { write_reg(self.emu, Register::Rdi, self.ret_val as u64) };
    }
    fn arg_to_stack(&mut self, idx: usize, offset: usize, size: usize) {
        let dst = (self.sp as usize + offset) as *mut u8;
        unsafe { std::ptr::copy_nonoverlapping(*self.vals.add(idx), dst, size) };
    }
    fn arg_to_reg(&mut self, idx: usize, reg: Register, byte_off: usize, size: usize) {
        let mut v = 0u64;
        unsafe {
            std::ptr::copy_nonoverlapping(
                (*self.vals.add(idx)).add(byte_off),
                (&raw mut v).cast(),
                size,
            );
            if is_xmm(reg) {
                write_xmm(self.emu, reg, Xmm { lo: v, hi: 0 });
            } else {
                write_reg(self.emu, reg, v);
            }
        }
    }
    fn call(&mut self) {
        unsafe { DynaCall(self.emu, self.func, 0) };
    }
    fn ret_from_reg(&mut self, reg: Register, byte_off: usize, size: usize) {
        assert!(!self.ret_val.is_null());
        let v = unsafe {
            if is_xmm(reg) {
                read_xmm(self.emu, reg).lo
            } else {
                read_reg(self.emu, reg)
            }
        };
        unsafe {
            std::ptr::copy_nonoverlapping((&raw const v).cast(), self.ret_val.add(byte_off), size)
        };
    }
}

// 生成函数体，不负责生成函数声明
// 函数声明我们假设为RetType stub_name(Type0 __arg0,Type1 __arg1,....)
// 我们假设有一个缓存的thread local的结构 Box64State box64_state
// 假设有align_down等辅助函数在头文件定义，RSP等宏定义,u64等类型...
// 我们假设有全局初始化的变量(通过sym) __func_name_sym
// 其中:struct Box64State {
//     regs: *mut u64,
//     xmm: *mut Xmm,
// }
struct CCodegenBackend {
    func: String,
    func_name: String,
}
impl CallBackend for CCodegenBackend {
    fn alloc_stack(&mut self, size: u64) {
        writeln!(
            self.func,
            "uint64_t __old_sp = box64_state.regs[RSP];\n\
             uint64_t __sp = align_down(__old_sp - {size}, 16);\n\
             box64_state.regs[RSP] = __sp;"
        )
        .expect("writeln error!");
    }
    fn restore_stack(&mut self) {
        writeln!(self.func, "box64_state.regs[RSP] = __old_sp;").expect("writeln error!");
    }
    fn setup_sret(&mut self) {
        writeln!(self.func, "box64_state.regs[RDI] = (u64)&__ret;").expect("writeln error!");
    }
    fn arg_to_stack(&mut self, idx: usize, offset: usize, size: usize) {
        writeln!(
            self.func,
            "memcpy((void*)(__sp + {offset}), &__arg{idx}, {size});"
        )
        .expect("writeln error!");
    }
    fn arg_to_reg(&mut self, idx: usize, reg: Register, byte_off: usize, size: usize) {
        if is_xmm(reg) {
            let i = (reg as u32) - (Register::Xmm0 as u32);
            writeln!(
                self.func,
                "box64_state.xmm[{i}].hi = 0;\n\
                 box64_state.xmm[{i}].lo = 0;\n\
                 memcpy(&box64_state.xmm[{i}].lo, (const char*)&__arg{idx} + {byte_off}, {size});"
            )
            .expect("writeln error!");
        } else {
            let r = reg as u32;
            writeln!(
                self.func,
                "box64_state.regs[{r}] = 0;\n\
                 memcpy(&box64_state.regs[{r}], (const char*)&__arg{idx} + {byte_off}, {size});"
            )
            .expect("writeln error!");
        }
    }
    fn call(&mut self) {
        writeln!(self.func, "box64dl_call(__{}_sym);", self.func_name).expect("writeln error!");
    }
    fn ret_from_reg(&mut self, reg: Register, byte_off: usize, size: usize) {
        if is_xmm(reg) {
            let i = (reg as u32) - (Register::Xmm0 as u32);
            writeln!(
                self.func,
                "memcpy((char*)&__ret + {byte_off}, &box64_state.xmm[{i}].lo, {size});"
            )
            .unwrap();
        } else {
            let r = reg as u32;
            writeln!(
                self.func,
                "memcpy((char*)&__ret + {byte_off}, &box64_state.regs[{r}], {size});"
            )
            .unwrap();
        }
    }
}
// 这个是C 代码生成器的入口
pub fn gen_stub(args: CFunc, func_name: &str) -> Result<String> {
    let mut plan: CallPlan = unsafe { std::mem::zeroed() };
    let status = abi_plan_gen(args, &mut plan);
    if status != 0 {
        bail!("abi_plan_gen failed with status {status}");
    }
    let slice = if plan.args_len == 0 {
        &[][..]
    } else {
        unsafe { core::slice::from_raw_parts(plan.args, plan.args_len) }
    };
    let mut b = CCodegenBackend {
        func: String::new(),
        func_name: func_name.to_owned(),
    };
    lower_call(&plan, slice, &mut b);
    unsafe { free(plan.args.cast()) };
    Ok(b.func)
}

fn gen_stub_with_check(args: CFunc, func_name: *const c_char) -> Result<*mut c_char> {
    ensure!(!func_name.is_null(), "func_name is null");
    let name = unsafe { CStr::from_ptr(func_name) }.to_str()?;
    let code = gen_stub(args, name)?;
    let ptr = unsafe { malloc(code.len() + 1) } as *mut c_char;
    ensure!(!ptr.is_null(), "malloc failed");
    unsafe {
        std::ptr::copy_nonoverlapping(code.as_ptr(), ptr.cast(), code.len());
        *ptr.add(code.len()) = 0;
    }
    Ok(ptr)
}
// C代码生成的入口，C风格包装

#[unsafe(no_mangle)]
unsafe extern "C" fn box64dl_gen_stub(args: CFunc, func_name: *const c_char) -> *mut c_char {
    gen_stub_with_check(args, func_name).unwrap_or(std::ptr::null_mut())
}

// args_val: void**，每项指向一个参数值；ret_val: 指向返回值缓冲区
fn call_plan(
    func: *mut c_void,
    plan: &CallPlan,
    args_val: *mut c_void,
    ret_val: *mut c_void,
) -> i32 {
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
    let mut b = ExecBackend {
        emu,
        func: func as usize,
        vals,
        ret_val: ret_val.cast(),
        old_sp: 0,
        sp: 0,
    };
    lower_call(plan, args, &mut b);
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
    let mut plan: CallPlan = unsafe { std::mem::zeroed() };
    let status = abi_plan_gen(args, &mut plan);
    if status != 0 {
        return status;
    }
    let ret = call_plan(func, &plan, args_val, ret_val);
    unsafe { free(plan.args.cast()) };
    ret
}
// JIT路径看起来会非常美妙，但是要等abi-plan全做好了....
//
// TODO: 实现 trait三个后端：
//   - Exec: 直接执行寄存器/栈操作
//   - CCodegen: 生成 C 桩代码
//   - Emit: 生成内存中的 host 机器码
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
