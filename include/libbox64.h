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
BOX64_API void box64_lib_fini(void);
BOX64_API int box64_lib_ready(void);

BOX64_API void* box64_dlopen(const char* path, int flags);
BOX64_API void* box64_dlsym(void* handle, const char* name);
BOX64_API int box64_dlclose(void* handle);
BOX64_API const char* box64_dlerror(void);

BOX64_API uint64_t box64_call(void* sym, const char* fmt, ...);
BOX64_API uint64_t box64_call_regs(void* sym, uint64_t* gregs, uint64_t* fregs);
/* gregs[6] + fregs[8] + stack blob (MEMORY / overflow args), then DynaCall. */
/* On return: gregs[0]=rax, gregs[1]=rdx; fregs[0]/[1]=xmm0/xmm1. */
BOX64_API uint64_t box64_call_marshaled(void* sym, uint64_t* gregs,
                                        uint64_t* fregs, const void* stack,
                                        size_t stack_len);
BOX64_API void* box64_callback(void* host_fn, const char* fmt);

#ifdef __cplusplus
}
#endif

#endif /* LIBBOX64_H */
