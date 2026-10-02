/*
 * libbox64dl: retired fault-routing entry point (patches/0020).
 *
 * This file is a GS-free compatibility shim and nothing else. It exists only
 * because patches/0003's seam is still referenced out of tree (tests, embedder
 * glue) and because removing a published symbol is a separate, breaking change.
 * It does no classification, holds no state, and reads no engine field.
 *
 * WHAT REPLACED IT
 * ----------------
 * patches/0003 asked "is this thread a guest?" by testing a segment register: a
 * wine TEB lives at the emu's GS base, so `emu->segs_offs[_GS] != 0` was read as
 * "guest context". That misroutes every plain Linux-x64 guest, whose TLS is in
 * FS and whose GS base is therefore 0: its dynarec-protected JIT faults were
 * handed to the host action instead of the engine's repairs. The test is gone,
 * and the engine's SIGSEGV/SIGBUS/SIGILL/SIGABRT entry no longer calls this
 * function at all, so it can no longer misroute a fault by itself.
 *
 * The single decision point is now:
 *
 *   src/signal_actions.c  the application signal action module, which owns the
 *                         four engine signals and decides
 *                         DEFAULT / IGNORE / NATIVE / GUEST from the real host
 *                         siginfo_t and ucontext_t and the kernel entry variant;
 *   src/libtools/signals.c (patches/0020)  box64dl_engine_signal_hook_impl(),
 *                         which runs the engine's hardware repairs and then
 *                         calls box64dl_signal_deliver().
 *
 * A user-generated signal (si_code <= 0), a hardware fault on a thread without
 * an emu, and a repairable guest hardware fault are all distinguished there, in
 * one place. An application's native or guest handler is never preceded by this
 * function.
 *
 * CONTRACT
 * --------
 * box64dl_signal_route() reports "not handled here": it returns 0 and does
 * nothing. It is NOT a forwarder and NOT an alternative entry point. A private
 * legacy caller that used it must call the real module entry instead -
 * box64dl_signal_deliver() with the engine-computed values, or, from inside the
 * engine, box64dl_engine_signal_hook(). Simply returning from a handler does not
 * make the kernel enter another handler: only the module's own installed
 * dispatcher performs the application decision. There is deliberately no
 * forwarding to a native or default action from inside this shim - that would
 * re-introduce the bypass patches/0020 removed.
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <signal.h>
#include <ucontext.h>

#include "libbox64_signal_actions.h"

#ifdef __cplusplus
extern "C" {
#endif

int box64dl_signal_route(x64emu_t* emu, int sig, siginfo_t* info, void* ucntx) {
  (void)emu;
  (void)sig;
  (void)info;
  (void)ucntx;
  /* Superseded by the application action module's decision, reached from the
   * engine hook. Never forwards anything itself. */
  return 0;
}

#ifdef __cplusplus
}
#endif
