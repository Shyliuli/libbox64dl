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

/*
 * ------------------------------------------------------------------
 * Leaving guest state on the current thread (Wine detach support)
 * ------------------------------------------------------------------
 *
 * A host pthread that was attached to a guest world (Wine's
 * __wine_attach_current_thread / __wine_adopt_thread) regains control
 * through the PE bridge's longjmp(). That unwind lands on the host stack, but
 * it bypasses the engine's own return path, so the thread's x64emu_t is left
 * "dirty":
 *
 *   - emu RSP still points into the guest thread's Wine PE stack (the TEB
 *     stack). The native host SP is fine, but every later box64_call_regs()
 *     on this thread starts its guest frame from there.
 *   - guest GS (emu->segs_offs[_GS]) still points at the Wine TEB.
 *
 * Wine's __wine_detach_current_thread() frees exactly those two mappings
 * (thread_data/TEB and the PE stack), and it deliberately refuses to run
 * while the *emulated* SP is inside them (its stack guard). So a detach
 * issued through box64_call_regs() with the dirty RSP is rejected with
 * STATUS_INVALID_PARAMETER, and even if it were not, the emu would survive
 * pointing into freed memory afterwards.
 *
 * box64_detach_current_thread_guest() is the minimal host-side repair:
 *
 *   - it repoints emu RSP to the top of this thread's Box64-owned emu stack
 *     (emu->init_stack + emu->size_stack, 16-byte aligned, the same value
 *     the engine's entry path uses), i.e. outside every Wine-owned mapping,
 *     so the unix detach entry's stack guard passes and DynaCall() has a
 *     live stack to use;
 *   - it clears the guest GS binding (emu->segs_offs[_GS] and emu->segs[_GS])
 *     so box64dl_signal_route() classifies the thread as host again instead
 *     of dispatching the guest's SEH on a freed TEB;
 *   - it frees NOTHING and allocates NOTHING. The former guest stack is
 *     Wine's to release in __wine_detach_current_thread(), and the Box64 emu
 *     stack belongs to the emu and stays mapped for the rest of the thread's
 *     life. No extra stack is leaked on either side.
 *
 * Ordering inside a Wine detach (the caller owns the sequence; libbox64dl only
 * provides the switch):
 *
 *   PE first    run the PE ntdll.dll!LdrShutdownThread on this thread through
 *               the engine, while the guest TEB / GS binding are still valid,
 *               so DLL_THREAD_DETACH, FLS destructors and PE TLS teardown run;
 *   then        this switch: clear guest GS and move the emulated RSP off the
 *               Wine stack;
 *   unix last   __wine_detach_current_thread(), which frees the TEB /
 *               thread_data / server thread object / GS binding.
 *
 * The PE shutdown is NOT part of __wine_detach_current_thread() and must not be
 * deferred to it: LdrShutdownThread has to run on the PE stack, and once this
 * switch has moved the emulated SP that stack is no longer in use. winedl
 * implements exactly this order (see winedl/include/winedl.h).
 *
 * Call timing (this is part of the contract, the switch is not idempotent):
 *
 *   1. run it from HOST code on the attached thread, after the PE bridge has
 *      longjmp()ed back -- never from inside a bridge callback or any other
 *      frame the guest is still using;
 *   2. in the detach order above, call it AFTER the PE LdrShutdownThread call
 *      and BEFORE the unix detach entry through box64_call_regs(); that
 *      entry's guard inspects the emulated SP, so the order cannot be reversed;
 *   3. if it returns non-zero, the emu stack could not be validated: do NOT
 *      call the unix detach entry, leave the thread attached and untouched;
 *   4. box64_detach_current_thread_guest_restore(saved) rolls back this switch
 *      only: it puts the guest RSP/GS back for a detach that failed before the
 *      PE shutdown ran. It cannot undo a PE LdrShutdownThread. Once the PE
 *      shutdown has run -- as in winedl's order, where it precedes this switch
 *      -- a failed unix detach leaves the thread spent and must NOT be restored
 *      or retried;
 *   5. if the unix detach succeeds, the thread is terminal-FOREIGN: keep the
 *      snapshot but do not restore it.
 *
 * Only guest state owned by libbox64dl is touched; Wine's own stack guard is
 * never relaxed.
 *
 * box64_prepare_detach_current_thread(saved) returns 0 when the thread now
 * has a Box64/host RSP and a zero guest GS, -1 when the engine is not ready
 * or the emu has no usable, writable Box64 stack (nothing is changed on -1).
 * `saved` may be NULL for a one-way switch; otherwise it receives the
 * pre-switch state and is zeroed when the thread had no emu (already host). A
 * thread with no emu is a success no-op: it has no guest state to leave.
 * box64_detach_current_thread_guest() is the same call under its earlier
 * spelling; both stay exported.
 */
typedef struct box64_guest_state_s {
  uint64_t rsp;     /* emu RSP before the switch                  */
  uint64_t gs_base; /* emu->segs_offs[_GS] before the switch      */
  uint16_t gs_sel;  /* emu->segs[_GS] before the switch           */
  uint16_t reserved[3];
} box64_guest_state_t;

BOX64_API int box64_prepare_detach_current_thread(box64_guest_state_t* saved);
BOX64_API int box64_detach_current_thread_guest(box64_guest_state_t* saved);
/* Undo a switch whose detach failed BEFORE any PE-side shutdown ran (step 4 of
 * the timing contract above); returns 0 on success, -1 if not ready or the
 * thread has no emu. It rolls back this switch only and does not undo a PE
 * LdrShutdownThread. Not valid after a successful detach, nor after a PE
 * LdrShutdownThread has run on the thread. */
BOX64_API int box64_detach_current_thread_guest_restore(
    const box64_guest_state_t* saved);

/*
 * ------------------------------------------------------------------
 * Returning to a clean guest stack (Wine attach / bridge longjmp)
 * ------------------------------------------------------------------
 *
 * A host pthread that became a Windows thread through Wine's
 * __wine_attach_current_thread regains control through the PE bridge's
 * longjmp(). That unwind lands on the host stack but bypasses DynaCall()'s own
 * register restore (dynarec/dynarec.c), so the thread's x64emu_t keeps the RSP
 * the guest PE thread-start code was standing on when it called the bridge: an
 * abandoned frame inside the Wine PE stack. Every later
 * box64_call_regs()/box64_call_marshaled() on that thread would build its guest
 * frame from that stale location.
 *
 * box64_reset_current_thread_guest_stack() is the minimal host-side repair:
 *
 *   - it repoints emu RSP to a clean call anchor just below `stack_top`:
 *     RSP = (stack_top & ~0xF) - 0x28. `stack_top` is the EXCLUSIVE end of the
 *     guest stack region (Wine's TIB.StackBase, or the Box64 emu stack end).
 *     With the anchor at top-0x28 the Win64 shadow space a callee may touch,
 *     [RSP+8, RSP+0x28), ends exactly at the top instead of crossing it, the
 *     return slot a call then pushes at RSP-8 is still inside the region, and
 *     RSP % 16 == 8 (the x86_64 entry parity both Win64 and SysV use, and the
 *     parity the post-longjmp RSP already carries);
 *   - it keeps the guest GS/TEB binding (emu->segs_offs[_GS] / emu->segs[_GS])
 *     exactly as it is: unlike box64_prepare_detach_current_thread(), this does
 *     NOT leave guest state. The thread stays a Wine thread with a valid TEB;
 *   - it frees nothing and allocates nothing.
 *
 * box64_current_thread_teb_stack_top() reads this thread's Wine TEB from the
 * emu's guest GS base and returns its TIB.StackBase (x86_64 NT_TIB: StackBase
 * at +0x08, StackLimit at +0x10) after validating the range.
 * box64_reset_current_thread_guest_stack_from_teb() is the two combined.
 *
 * Call timing, from HOST code on the current thread, BETWEEN guest calls:
 * after the bridge longjmp() has returned, or immediately before a new
 * box64_call_regs()/box64_call_marshaled(). Never from inside a bridge callback
 * or any other frame the guest is still using -- that would move the stack out
 * from under the running call. Calling it twice in a row is harmless.
 *
 * Return contract: 0 on success, including "this thread has no emu" (a plain
 * host thread has no guest stack to reset; no error is recorded). -1 with
 * box64_dlerror() set when box64 is not ready, `stack_top` is unusable, or
 * (from_teb) this thread has no usable Wine TEB. On -1 nothing is changed.
 */
BOX64_API int box64_reset_current_thread_guest_stack(uint64_t stack_top);
BOX64_API int box64_current_thread_teb_stack_top(uint64_t* top);
BOX64_API int box64_reset_current_thread_guest_stack_from_teb(void);

#ifdef __cplusplus
}
#endif

#endif /* LIBBOX64_EXT_H */
