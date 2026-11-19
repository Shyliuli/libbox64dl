// SPDX-License-Identifier: MIT
#include <stddef.h>
#include <stdint.h>
#include <unistd.h>

#include "auxval.h"
#include "box64context.h"
#include "debug.h"
#include "env.h"
#include "guestalloc.h"
#include "sysinfo.h"
#include "x64emu_private.h"

extern char **environ;

int global_init(void)
{
    //TODO: 这里理论上需要串行，且只能调用一次
    init_guestalloc();
    if (init_auxval(0, NULL, environ))
        return -1;

    box64_pagesize = sysconf(_SC_PAGESIZE);
    if (!box64_pagesize)
        box64_pagesize = 4096;

    LoadEnvVariables();

    //TODO: 这里需要一把锁（首次初始化）
    box64context_t* ctx = NewBox64Context(1);
    if (!ctx)
        return -1;
    ctx->argv[0] = "box64dl";
    ctx->envv = environ;
    for (ctx->envc = 0; environ[ctx->envc]; ++ctx->envc) {}

    InitializeEnvFiles();
    InitializeSystemInfo();
    return 0;
}

int thread_init(void)
{
    // 这里需要 per-thread 调用
    if (!thread_get_emu())
        return -1;
    return 0;
}

void box64_set_r8(x64emu_t* emu, uint64_t value)
{
    R_R8 = value;
}

void box64_set_r9(x64emu_t* emu, uint64_t value)
{
    R_R9 = value;
}
