/* Public embed API — host (native) side loads/calls x86_64 ELF via box64. */
#ifndef LIBBOX64_H
#define LIBBOX64_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef BOX64_API
#define BOX64_API __attribute__((visibility("default")))
#endif

BOX64_API int box64_lib_init(void);
BOX64_API int box64_main(int argc, const char** argv, char** env);
BOX64_API void box64_lib_fini(void);
BOX64_API int box64_lib_ready(void);
/* Rust facade owns the ready state: returns whether its BOX64_LIB Option is
 * Some. C API guards (dlsym/dlclose/call) use this instead of a C-side flag. */
BOX64_API int box64_is_ready(void);
/* Ensure the current thread has a properly sized guest emu (8 MiB stack),
 * instead of box64's 256 KiB fallback used by RunFunctionFmt. */
BOX64_API int box64_ensure_emu(void);

BOX64_API void* box64_dlopen(const char* path, int flags);
BOX64_API void* box64_dlsym(void* handle, const char* name);
BOX64_API int box64_dlclose(void* handle);
BOX64_API const char* box64_dlerror(void);

BOX64_API uint64_t box64_call_regs(void* sym, uint64_t* gregs, uint64_t* fregs);
/* gregs[6] + fregs[8] + stack blob (MEMORY / overflow args), then DynaCall. */
/* On return: gregs[0]=rax, gregs[1]=rdx; fregs[0]/[1]=xmm0/xmm1. */
BOX64_API uint64_t box64_call_marshaled(void* sym, uint64_t* gregs,
                                        uint64_t* fregs, const void* stack,
                                        size_t stack_len);
/* Host function -> guest-callable address, six integer registers forwarded. */
BOX64_API void* box64_callback(void* host_fn, const char* fmt);

/*
 * Same, but the caller supplies the wrapper that runs on the guest's behalf:
 *
 *   void my_bridge(void* emu, uintptr_t host_fn);
 *
 * It gets the emulator handle and the host function, and moves arguments and
 * the return value itself with the two accessors below. This is how a callback
 * whose parameters are not all integers gets described — the default wrapper
 * only knows how to forward six integer registers.
 */
BOX64_API void* box64_callback_with(void* host_fn, void* bridge_fn);

/* Argument registers as the guest left them: 6 integer, 8 SSE. */
BOX64_API void box64_bridge_args(void* emu, uint64_t* gregs, uint64_t* fregs);
/* Return registers the guest will read: rax, rdx, xmm0, xmm1. */
BOX64_API void box64_bridge_ret(void* emu, uint64_t rax, uint64_t rdx,
                                const uint64_t* fregs2);

#ifdef __cplusplus
}
#endif

#endif /* LIBBOX64_H */
