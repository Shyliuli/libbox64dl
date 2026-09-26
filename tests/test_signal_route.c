/*
 * Regression for src/signal_route.c: which faults the embedded engine claims
 * for itself and which are handed to the host's pre-box64 action.
 *
 * This is a SEPARATE concern from the per-call signal-mask policy
 * (tests/test_signal_policy.c). Routing decides whether box64 owns the fault
 * at all; the mask policy decides what the handler does once it does.
 *
 * Cases (all real signals):
 *   A. host fault: raise(SIGSEGV) on a thread whose faulting PC is host code
 *      must reach the pre-box64 host action (the V8-wasm-trap shape).
 *   C. host fault inside a bridge callback, while a guest call is active (so
 *      emu->jmpbuf is armed): the faulting PC is host code, so it must still go
 *      to the host action, not to box64's guest dispatch.
 *
 * A plain GS=0 Linux guest's dynarec-protected JIT fault is a KNOWN GAP in this
 * embedded tree (the route marker is the wine TEB in GS); it is NOT fixed here
 * and there is deliberately no fake-TEB fixture. See README "fault 路由".
 *
 * C abandons the engine's C frame (the host action siglongjmp()s out of the
 * signal frame), so it runs LAST and the process exits right after checking it.
 * box64_lib_fini()'s handler restoration is covered by tests/test_signal_policy.c.
 *
 * Usage: test_signal_route <guest.so>
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <dlfcn.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ucontext.h>
#include <unistd.h>

#include "libbox64.h"
#include "call_compat.h"

static int g_fail;
static int g_checks;

static void check(int cond, const char* what) {
  g_checks++;
  if (!cond) {
    g_fail = 1;
    printf("[signal-route] FAIL %s\n", what);
  } else {
    printf("[signal-route] ok   %s\n", what);
  }
}

/* --- the pre-box64 host action -------------------------------------------- */

static volatile sig_atomic_t g_host_hits;
static volatile sig_atomic_t g_host_longjmp; /* 1: unwind out (case C) */
static sigjmp_buf g_host_jmp;

static void host_segv(int sig, siginfo_t* info, void* ucntx) {
  (void)sig;
  g_host_hits++;
  if (g_host_longjmp) siglongjmp(g_host_jmp, 1);

  /*
   * In case A the delivery is a raise() (SI_TKILL) and returning is correct.
   * A real hardware fault reaching the host action where the test did not ask
   * for it means box64 failed to claim a JIT fault: diagnose and stop instead
   * of letting the instruction re-execute forever.
   */
  if (info && info->si_code != SI_USER && info->si_code != SI_TKILL &&
      info->si_code != SI_QUEUE) {
    void* pc = ucntx ? (void*)((ucontext_t*)ucntx)->uc_mcontext.__pc : NULL;
    fprintf(stderr,
            "[signal-route] FATAL: hardware fault (code=%d) at pc=%p addr=%p "
            "reached the host action unasked (wrong route)\n",
            info->si_code, pc, info->si_addr);
    fflush(NULL);
    _exit(4);
  }
  (void)ucntx;
}

static void install_host_segv(void) {
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_sigaction = host_segv;
  sa.sa_flags = SA_SIGINFO;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGSEGV, &sa, NULL);
}

/* --- guest ---------------------------------------------------------------- */

static void* g_lib;
static void* g_invoke;

static void* sym(const char* name) { return box64_dlsym(g_lib, name); }

/* Host code that faults while a guest call is active: dereferences the value
 * the guest passed (1 => unmapped), on purpose. */
static uint64_t host_cb_bad(uint64_t a, uint64_t b, uint64_t c, uint64_t d,
                            uint64_t e, uint64_t f) {
  (void)b;
  (void)c;
  (void)d;
  (void)e;
  (void)f;
  return *(volatile uint64_t*)(uintptr_t)a;
}

int main(int argc, char** argv) {
  if (argc < 2) {
    printf("usage: %s <guest.so>\n", argv[0]);
    return 2;
  }
  box64_diag_set(0);
  install_host_segv();

  if (box64_lib_init() != 0 || box64_ensure_emu() != 0) {
    printf("[signal-route] engine init: %s\n",
           box64_dlerror() ? box64_dlerror() : "?");
    return 1;
  }

  /* Case A: host PC, not a guest call. */
  g_host_hits = 0;
  g_host_longjmp = 0;
  raise(SIGSEGV);
  check(g_host_hits == 1, "A: host raise(SIGSEGV) reaches the host action");

  g_lib = box64_dlopen(argv[1], RTLD_NOW);
  if (!g_lib) {
    printf("[signal-route] dlopen: %s\n",
           box64_dlerror() ? box64_dlerror() : "?");
    return 1;
  }
  g_invoke = sym("g_invoke");
  if (!g_invoke) {
    printf("[signal-route] missing guest symbols\n");
    return 1;
  }

  /* Case C (last): host fault from a bridge callback while jmpbuf is armed. */
  {
    void* cb = box64_callback((void*)host_cb_bad);
    check(cb != NULL, "C: callback bridge registered");
    if (cb) {
      g_host_hits = 0;
      g_host_longjmp = 1;
      if (sigsetjmp(g_host_jmp, 1) == 0) {
        (void)test_simple2(g_invoke, (uint64_t)(uintptr_t)cb, 1);
        check(0, "C: host callback fault unwound to the host action");
      } else {
        check(1, "C: host callback fault unwound to the host action");
        check(g_host_hits == 1,
              "C: host callback fault went to the host action, not box64");
      }
    }
  }

  if (g_fail) {
    printf("[signal-route] RESULT: FAIL (%d checks)\n", g_checks);
    fflush(NULL);
    _exit(1);
  }
  printf("[signal-route] RESULT: PASS (%d checks)\n", g_checks);
  /* Case C left the engine mid-call on purpose; skip destructors, but flush so
   * the RESULT/checks are not lost (stdout to a file is block-buffered). */
  fflush(NULL);
  _exit(0);
}
