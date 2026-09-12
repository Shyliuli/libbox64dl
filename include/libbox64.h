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

/*
 * Run `sym` through the engine with the given register file and return the
 * guest's RAX.
 *
 * ERROR CONTRACT: the return value is the guest's result, so 0 is a legitimate
 * return. An engine-level refusal (not initialized, NULL argument, no usable
 * per-thread emu) ALSO returns 0, so it is distinguishable ONLY through the
 * thread-local error slot: every refusal leaves box64_dlerror() set, and a call
 * that actually ran clears it. Callers that treat 0 as success (an NTSTATUS
 * check, say) must read box64_dlerror() right after the call. Like POSIX
 * dlerror, the slot is read-and-consume.
 */
BOX64_API uint64_t box64_call_regs(void* sym, uint64_t* gregs, uint64_t* fregs);
/* gregs[6] + fregs[8] + stack blob (MEMORY / overflow args), then DynaCall. */
/* On return: gregs[0]=rax, gregs[1]=rdx; fregs[0]/[1]=xmm0/xmm1. */
/* Same 0/error ambiguity and error-slot contract as box64_call_regs(). */
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

/*
 * Declare a mapping the EMBEDDER made directly (host mmap/mprotect) as guest
 * memory with the given protection.
 *
 * box64 keeps its own protection map and its interpreter's CheckExec() refuses
 * to run guest code on a page that map does not describe as PROT_READ|PROT_EXEC
 * (it raises a synthetic SIGSEGV, code 0xecec). Mappings created by box64
 * itself, and PE/ELF images wine loads through the guest side, are recorded
 * automatically; a guest-addressed image the embedder maps with the raw host
 * mmap is not — e.g. winedl's in-memory main image, whose export stubs the
 * guest jumps into. This publishes that range so the stubs are executable.
 *
 * `prot` is in the host PROT_* encoding and is recorded VERBATIM, exactly like
 * box64's own setProtection(): this call does not map, mprotect or probe any
 * page. The CALLER must therefore ensure the host pages really carry `prot`
 * before declaring it — an mprotect may be refused (LSM, seccomp, W^X policy)
 * without the syscall failing, and a declared-but-absent PROT_EXEC turns a loud
 * syscall error into a silent guest fault at the first branch. Verify the
 * effective protection out of band (e.g. /proc/self/maps) and treat a mismatch
 * as fatal rather than declaring an image that cannot run.
 *
 * Returns 0, or -1 (with box64_dlerror() set) when the engine is not ready,
 * an argument is NULL/empty, or `prot` is 0 (which would publish nothing).
 */
BOX64_API int box64_declare_host_mapping(void* addr, size_t size, uint32_t prot);

/* Argument registers as the guest left them: 6 integer, 8 SSE. */
BOX64_API void box64_bridge_args(void* emu, uint64_t* gregs, uint64_t* fregs);
/* Return registers the guest will read: rax, rdx, xmm0, xmm1. */
BOX64_API void box64_bridge_ret(void* emu, uint64_t rax, uint64_t rdx,
                                const uint64_t* fregs2);

#ifdef __cplusplus
}
#endif

#endif /* LIBBOX64_H */
