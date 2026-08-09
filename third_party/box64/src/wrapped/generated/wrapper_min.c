#include "box64context.h"

// Minimal forward declarations; the real wrapper_t type is opaque for CPU backend use.
struct x64emu_s;
typedef void (*wrapper_t)(struct x64emu_s* emu, uintptr_t start_addr, void* real_addr);

int isSimpleWrapper(wrapper_t fun)
{
    (void)fun;
    return 0;
}

int isRetX87Wrapper(wrapper_t fun)
{
    (void)fun;
    return 0;
}

int isRetX87Wrapper32(wrapper_t fun)
{
    (void)fun;
    return 0;
}
