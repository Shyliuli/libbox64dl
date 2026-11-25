#include <box64.h>
#include <test.h>

static void *lib;
static void *sym_add;
static void *sym_twice;
static void *sym_addf;
static void *sym_sum9;
static void *sym_incr;

static void *lookup(const char *name)
{
    void *address = box64dl_sym(lib, name);
    if (!address)
        FAIL("dlsym %s failed\n", name);
    return address;
}

static void init(void)
{
    box64dl_init();
    lib = box64dl_dlopen("./libadder.so", BOX64DL_RTLD_NOW | BOX64DL_RTLD_GLOBAL);
    if (!lib)
        FAIL("dlopen failed\n");

    sym_add = lookup("add");
    sym_twice = lookup("twice");
    sym_addf = lookup("addf");
    sym_sum9 = lookup("sum9");
    sym_incr = lookup("incr");
}

/* ------------------------------------------------------------------
 * 静态路径：plan 是编译期常量数据，运行时只有一次 box64dl_static_call。
 * 下面的内容就是以后 codegen 应该产出的形态。
 * ------------------------------------------------------------------ */

static Register add_arg_regs[2][1] = {
    {Register_Rdi},
    {Register_Rsi},
};
static Place add_args[2] = {
    {.size = 4,
     .align = 4,
     .alloc = {.tag = Allocation_Reg, .reg = {.reg = add_arg_regs[0], .len = 1}}},
    {.size = 4,
     .align = 4,
     .alloc = {.tag = Allocation_Reg, .reg = {.reg = add_arg_regs[1], .len = 1}}},
};
static Register int_ret_reg[1] = {Register_Rax};
static const CallPlan add_plan = {
    .ret = {.size = 4,
            .align = 4,
            .alloc = {.tag = Allocation_Reg, .reg = {.reg = int_ret_reg, .len = 1}}},
    .args = add_args,
    .args_len = 2,
    .stack_size = 0,
};

static Place twice_args[1] = {
    {.size = 4,
     .align = 4,
     .alloc = {.tag = Allocation_Reg, .reg = {.reg = add_arg_regs[0], .len = 1}}},
};
static const CallPlan twice_plan = {
    .ret = {.size = 4,
            .align = 4,
            .alloc = {.tag = Allocation_Reg, .reg = {.reg = int_ret_reg, .len = 1}}},
    .args = twice_args,
    .args_len = 1,
    .stack_size = 0,
};

static Register addf_arg_regs[2][1] = {
    {Register_Xmm0},
    {Register_Xmm1},
};
static Place addf_args[2] = {
    {.size = 4,
     .align = 4,
     .alloc = {.tag = Allocation_Reg, .reg = {.reg = addf_arg_regs[0], .len = 1}}},
    {.size = 4,
     .align = 4,
     .alloc = {.tag = Allocation_Reg, .reg = {.reg = addf_arg_regs[1], .len = 1}}},
};
static Register float_ret_reg[1] = {Register_Xmm0};
static const CallPlan addf_plan = {
    .ret = {.size = 4,
            .align = 4,
            .alloc = {.tag = Allocation_Reg, .reg = {.reg = float_ret_reg, .len = 1}}},
    .args = addf_args,
    .args_len = 2,
    .stack_size = 0,
};

/* 前 6 个参数进寄存器，后 3 个走栈：offset 0/8/16，栈大小按 16 对齐到 32。 */
static Register sum9_arg_regs[6][1] = {
    {Register_Rdi},
    {Register_Rsi},
    {Register_Rdx},
    {Register_Rcx},
    {Register_R8},
    {Register_R9},
};
static Place sum9_args[9] = {
    {.size = 4,
     .align = 4,
     .alloc = {.tag = Allocation_Reg, .reg = {.reg = sum9_arg_regs[0], .len = 1}}},
    {.size = 4,
     .align = 4,
     .alloc = {.tag = Allocation_Reg, .reg = {.reg = sum9_arg_regs[1], .len = 1}}},
    {.size = 4,
     .align = 4,
     .alloc = {.tag = Allocation_Reg, .reg = {.reg = sum9_arg_regs[2], .len = 1}}},
    {.size = 4,
     .align = 4,
     .alloc = {.tag = Allocation_Reg, .reg = {.reg = sum9_arg_regs[3], .len = 1}}},
    {.size = 4,
     .align = 4,
     .alloc = {.tag = Allocation_Reg, .reg = {.reg = sum9_arg_regs[4], .len = 1}}},
    {.size = 4,
     .align = 4,
     .alloc = {.tag = Allocation_Reg, .reg = {.reg = sum9_arg_regs[5], .len = 1}}},
    {.size = 4, .align = 4, .alloc = {.tag = Allocation_Stack, .stack = {.offset = 0}}},
    {.size = 4, .align = 4, .alloc = {.tag = Allocation_Stack, .stack = {.offset = 8}}},
    {.size = 4, .align = 4, .alloc = {.tag = Allocation_Stack, .stack = {.offset = 16}}},
};
static const CallPlan sum9_plan = {
    .ret = {.size = 4,
            .align = 4,
            .alloc = {.tag = Allocation_Reg, .reg = {.reg = int_ret_reg, .len = 1}}},
    .args = sum9_args,
    .args_len = 9,
    .stack_size = 32,
};

static Register incr_arg_regs[1][1] = {Register_Rdi};
static Place incr_args[1] = {
    {.size = 8,
     .align = 8,
     .alloc = {.tag = Allocation_Reg, .reg = {.reg = incr_arg_regs[0], .len = 1}}},
};
static const CallPlan incr_plan = {
    .ret = {.size = 0, .align = 1, .alloc = {.tag = Allocation_Void}},
    .args = incr_args,
    .args_len = 1,
    .stack_size = 0,
};

static int add(int a, int b)
{
    const void *val[2] = {&a, &b};
    int32_t result = 0;
    AbiPlanError error = box64dl_static_call(sym_add, &add_plan, val, &result);
    if (error != AbiPlanError_Ok)
        FAIL("add call failed: %d\n", error);
    return result;
}

static int twice(int x)
{
    const void *val[1] = {&x};
    int32_t result = 0;
    AbiPlanError error = box64dl_static_call(sym_twice, &twice_plan, val, &result);
    if (error != AbiPlanError_Ok)
        FAIL("twice call failed: %d\n", error);
    return result;
}

static float addf(float a, float b)
{
    const void *val[2] = {&a, &b};
    float result = 0;
    AbiPlanError error = box64dl_static_call(sym_addf, &addf_plan, val, &result);
    if (error != AbiPlanError_Ok)
        FAIL("addf call failed: %d\n", error);
    return result;
}

static int sum9(int a1, int a2, int a3, int a4, int a5, int a6, int a7, int a8, int a9)
{
    const void *val[9] = {&a1, &a2, &a3, &a4, &a5, &a6, &a7, &a8, &a9};
    int32_t result = 0;
    AbiPlanError error = box64dl_static_call(sym_sum9, &sum9_plan, val, &result);
    if (error != AbiPlanError_Ok)
        FAIL("sum9 call failed: %d\n", error);
    return result;
}

static void incr(int *p)
{
    const void *val[1] = {&p};
    AbiPlanError error = box64dl_static_call(sym_incr, &incr_plan, val, NULL);
    if (error != AbiPlanError_Ok)
        FAIL("incr call failed: %d\n", error);
}

/* ---------------- 动态路径：运行时由 CType 生成 plan ---------------- */

static int add_dyn(int a, int b)
{
    CType arg_types[2] = {
        {.tag = CType_Int, .int_ = {.len = 4, .is_signed = true}},
        {.tag = CType_Int, .int_ = {.len = 4, .is_signed = true}},
    };
    CType ret_type = {.tag = CType_Int, .int_ = {.len = 4, .is_signed = true}};
    const void *val[2] = {&a, &b};
    int32_t result = 0;
    AbiPlanError error = box64dl_call(sym_add, arg_types, 2, val, ret_type, &result);
    if (error != AbiPlanError_Ok)
        FAIL("add_dyn call failed: %d\n", error);
    return result;
}

int main(void)
{
    init();

    if (add(20, 22) != 42)
        FAIL("add result wrong\n");
    if (twice(21) != 42)
        FAIL("twice result wrong\n");
    if (addf(1.5f, 2.25f) != 3.75f)
        FAIL("addf result wrong\n");
    if (sum9(1, 2, 3, 4, 5, 6, 7, 8, 9) != 45)
        FAIL("sum9 result wrong\n");

    int value = 41;
    incr(&value);
    if (value != 42)
        FAIL("incr result wrong\n");

    if (add_dyn(20, 22) != 42)
        FAIL("add_dyn result wrong\n");

    box64dl_close(lib);
    return 0;
}
