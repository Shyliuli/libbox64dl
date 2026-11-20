#include <box64.h>
#include <test.h>

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

    static const Plan plan = {.args = {Rdi, Rsi, None}, .result = Rax};
    uint64_t args[2] = {(uint64_t)a, (uint64_t)b};
    int32_t result = 0;
    AbiPlanError err = box64dl_call(sym, &plan, args, &result);
    if (err != Ok)
        FAIL("add call failed: %d\n", err);
    return result;
}

static int twice(int x)
{
    static void* sym;
    if (!sym) {
        sym = box64dl_sym(lib, "twice");
        if (!sym)
            FAIL("dlsym twice failed\n");
    }

    static const Plan plan = {.args = {Rdi, None}, .result = Rax};
    uint64_t args[1] = {(uint64_t)x};
    int32_t result = 0;
    AbiPlanError err = box64dl_call(sym, &plan, args, &result);
    if (err != Ok)
        FAIL("twice call failed: %d\n", err);
    return result;
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
