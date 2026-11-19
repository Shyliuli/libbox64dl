#include <box64dl.h>

static void* lib;

static void init(void)
{
    box64dl_init();
    lib = box64dl_dlopen("./libadder.so", BOX64DL_RTLD_NOW | BOX64DL_RTLD_GLOBAL);
    if (!lib)
        FAIL("dlopen failed\n");
}

static int add(int a, int b)
{
    static void* sym;
    if (!sym) {
        sym = box64dl_sym(lib, "add");
        if (!sym)
            FAIL("dlsym add failed\n");
    }

    CType types[2] = {C_INT, C_INT};
    Plan plan;
    // TODO: 我们应该在编译期执行解析逻辑，在这里留一个结构体就好
    AbiPlanError err = box64dl_get_plan(types, 2, C_INT, &plan);
    if (err != PLAN_OK)
        FAIL("add get_plan failed: %d\n", err);

    uint64_t args[2] = {(uint64_t)a, (uint64_t)b};
    uint64_t result = 0;
    err = box64dl_call(sym, &plan, args, &result);
    if (err != PLAN_OK)
        FAIL("add call failed: %d\n", err);
    return (int)result;
}

static int twice(int x)
{
    static void* sym;
    if (!sym) {
        sym = box64dl_sym(lib, "twice");
        if (!sym)
            FAIL("dlsym twice failed\n");
    }

    CType types[1] = {C_INT};
    Plan plan;
    // TODO: 我们应该在编译期执行解析逻辑，在这里留一个结构体就好
    AbiPlanError err = box64dl_get_plan(types, 1, C_INT, &plan);
    if (err != PLAN_OK)
        FAIL("twice get_plan failed: %d\n", err);

    uint64_t args[1] = {(uint64_t)x};
    uint64_t result = 0;
    err = box64dl_call(sym, &plan, args, &result);
    if (err != PLAN_OK)
        FAIL("twice call failed: %d\n", err);
    return (int)result;
}

int main(void)
{
    init();
    // 目前是手写add和twice，应该基于头文件生成.
    if (add(20, 22) != 42)
        FAIL("add result wrong\n");
    if (twice(21) != 42)
        FAIL("twice result wrong\n");

    box64dl_close(lib);
    return 0;
}
