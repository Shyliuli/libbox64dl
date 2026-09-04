/*
 * libbox64dl: host-context fault routing for embedded mode.
 *
 * box64's signal handler routes every fault to the guest's handler when one
 * is registered. In embedded mode that is only correct on threads that carry
 * a live guest thread context: wine's TEB lives at the emu's GS base, and
 * every thread wine created (or the boot executor) has it. A host thread —
 * the JS main thread, libuv workers, anything the host process runs — either
 * has no emu of its own or an ensure_emu one with a zero GS base; emulating
 * the guest's SEH dispatch on such a thread walks garbage and faults again
 * inside the handler, wedging the thread forever (observed as an infinite
 * fault-restart loop; standalone box64 never sees this because all of its
 * threads are its own). Patches/0003 calls box64dl_signal_route() before
 * box64 commits to the guest route, and saves the pre-box64 host actions at
 * init so a genuine host fault reaches the host's own handlers (V8's wasm
 * trap handler among them) instead of the guest's.
 *
 * This file is grafted into the vendored box64 build (same compile flags),
 * so the emu layout is the real one. Return 1 = handled here, box64 must
 * not touch it further; return 0 = a guest-context fault, box64 proceeds
 * with its own dispatch as before.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <ucontext.h>

#include "emu/x64emu_private.h"
#include "regs.h"
#include "x64emu.h"

/* loongarch: glibc spells the PC field __pc (see signal32.c's CONTEXT_PC). */
#define ROUTE_PC(U) ((U)->uc_mcontext.__pc)

/* patches/0003: the host actions installed BEFORE init_signal_helper took
 * the signals (SIG_DFL/SIG_IGN if the host had none). */
extern struct sigaction box64dl_pre_segv, box64dl_pre_bus, box64dl_pre_ill,
    box64dl_pre_abrt;

/* patches/0003 (threads.c): this thread's own emu, or NULL when the thread
 * never registered one — never fabricates, unlike thread_get_emu(). */
extern x64emu_t* thread_peek_emu(void);

static struct sigaction* pre_box64(int sig) {
  switch (sig) {
    case SIGSEGV: return &box64dl_pre_segv;
    case SIGBUS: return &box64dl_pre_bus;
    case SIGILL: return &box64dl_pre_ill;
    case SIGABRT: return &box64dl_pre_abrt;
  }
  return NULL;
}

int box64dl_signal_route(x64emu_t* emu, int sig, siginfo_t* info, void* ucntx) {
  struct sigaction* pre;

  if (getenv("BOX64DL_ROUTE_TRACE"))
    fprintf(stderr, "[route] sig=%d emu=%p gs=%#lx tid=%ld\n", sig, (void*)emu,
            emu ? (unsigned long)emu->segs_offs[_GS] : 0ul,
            (long)syscall(SYS_gettid));

  /* A thread with a wine TEB (GS base set) is inside the guest world: the
   * guest's own handler — wine's SEH — is the correct route, as before. */
  if (emu && emu->segs_offs[_GS] != 0) return 0;

  pre = pre_box64(sig);
  if (!pre) return 0;

  /* Say where, before any forwarding — the fault must be diagnosable even
   * when a host handler takes over (systemd-coredump tends to miss huge
   * embedded processes). */
  {
    Dl_info di;
    void* pc = (void*)ROUTE_PC((ucontext_t*)ucntx);
    const char* mod = (dladdr(pc, &di) && di.dli_fname) ? di.dli_fname : "?";
    fprintf(stderr,
            "[libbox64dl] host fault on thread %ld: sig=%d pc=%p (%s +%#lx) "
            "addr=%p\n",
            (long)syscall(SYS_gettid), sig, pc, mod,
            di.dli_saddr ? (unsigned long)(pc - di.dli_saddr) : 0ul,
            info ? info->si_addr : NULL);
  }

  /* Hand the fault to whatever the host had installed before box64: a real
   * host handler (V8's wasm trap handler chains to its own predecessors)
   * either recovers or terminates properly. */
  if ((pre->sa_flags & SA_SIGINFO) &&
      pre->sa_sigaction && pre->sa_sigaction != (void (*)(int, siginfo_t*, void*))SIG_DFL &&
      pre->sa_sigaction != (void (*)(int, siginfo_t*, void*))SIG_IGN) {
    pre->sa_sigaction(sig, info, ucntx);
    return 1;
  }
  if (!(pre->sa_flags & SA_SIGINFO) && pre->sa_handler &&
      pre->sa_handler != SIG_DFL && pre->sa_handler != SIG_IGN) {
    pre->sa_handler(sig);
    return 1;
  }

  /* No host handler: die loudly with the default disposition instead of
   * wedging. */
  signal(sig, SIG_DFL);
  raise(sig);
  return 1;
}
