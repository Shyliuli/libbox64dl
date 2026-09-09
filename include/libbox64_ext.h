/*
 * libbox64_ext — additive extensions to the public embed API (libbox64.h).
 *
 * The core header is treated as a frozen surface for embedders; additions
 * live in their own header instead. Same rule as libbox64.h: additions
 * only, existing declarations never change meaning.
 */
#ifndef LIBBOX64_EXT_H
#define LIBBOX64_EXT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef BOX64_API
#define BOX64_API __attribute__((visibility("default")))
#endif

/*
 * Name the box64 binary that re-execs the embedded engine's guest
 * children.
 *
 * Standalone box64 derives box64path from its own argv[0]: when a guest
 * execve/posix_spawn's another x86_64 ELF (wineserver, wineboot, ...), the
 * wrappers re-exec that child THROUGH box64 and need a box64 to point at.
 * The embedded context has no argv[0] to derive one from, so libbox64dl
 * resolves it in init_guest in this order:
 *
 *   1. this setter, if it was called — recommended for embedders that
 *      know their proven box64, e.g. the very one they already use to run
 *      helper processes;
 *   2. $LIBBOX64DL_BOX64PATH;
 *   3. a search of $PATH for an executable "box64";
 *   4. NULL — previous behaviour: guest exec of x86_64 children stays
 *      unsupported (nothing regresses).
 *
 * Call it before box64_lib_init() to steer the choice at context creation;
 * a call after init also patches the live context immediately. The string
 * is copied; previous copies are intentionally kept for the process
 * lifetime (the context may still point at them). Returns 0, or -1 when
 * given NULL / out of memory.
 */
BOX64_API int box64_set_box64path(const char* path);
BOX64_API void box64_set_wine_mode(void);

/*
 * Guest RSP as a bridge callback was entered — the stack sibling of
 * box64_bridge_args()/box64_bridge_ret(). `emu` is the opaque handle a
 * bridge function receives from box64_callback_with().
 *
 * Contract at the returned address, in the guest's (SysV) view:
 *   [rsp+0]  the return address the guest's call pushed;
 *   [rsp+8, ...) stack-passed arguments (SysV: 7th integer argument
 *            onwards), 8 bytes each, in declaration order.
 * A consumer interpreting a different guest ABI must translate
 * accordingly — e.g. a Win64 view over the same engine sees the caller's
 * 32-byte shadow space at [rsp+8, rsp+0x28) and stack arguments starting
 * at rsp+0x28.
 */
BOX64_API uint64_t box64_bridge_sp(void* emu);

#ifdef __cplusplus
}
#endif

#endif /* LIBBOX64_EXT_H */
