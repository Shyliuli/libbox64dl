/*
 * Real-signal regression for the per-call signal-mask policy on the
 * route-independent guest-signal resume path.
 *
 * tests/guest/sig_guest.c installs a NON-fatal (SIGUSR1) handler with a
 * NON-empty sa_mask (SIGALRM) and WITHOUT SA_NODEFER; the handler perturbs a
 * dead register (R11) in its own ucontext. box64's my_sigactionhandler_oldcode
 * sees the context change and resumes the emulator with
 * siglongjmp(emu->jmpbuf) — one of the signals.c sites the FAST policy changes.
 * This path does NOT go through box64dl_signal_route (which only handles the
 * four fatal signals), so it is reachable on a plain Linux-x64 guest thread
 * with GS base 0, with no fake TEB and no deferred route patch.
 *
 * Asserted, with REAL delivered signals:
 *   - the handler really ran and the guest resumed after the raising syscall;
 *   - entering the handler adds SIGALRM (sa_mask) and SIGUSR1 (kernel
 *     auto-block, no SA_NODEFER); after the signal-driven resume NEITHER may
 *     leak, while a mask bit set before the call (SIGUSR2) must survive;
 *   - the same holds for a repeated fire, for a fire raised from a host bridge
 *     re-entered inside an outer guest call (nested jmpbuf), and for worker
 *     threads on their own emus;
 *   - COMPAT arms with savemask=1 and does not use the restore helper; FAST
 *     arms with savemask=0 and the slow-path restore really ran.
 *
 * Policy is one-shot per process, so the Makefile starts one process per policy
 * (and once per dynarec/interpreter mode).
 *
 * Usage: test_guest_signal <sig_guest.so>
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <dlfcn.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "libbox64.h"

static int g_fail;
static int g_checks;

static void check(int cond, const char* what) {
  g_checks++;
  if (!cond) {
    g_fail = 1;
    printf("[guest-signal] FAIL %s\n", what);
  } else {
    printf("[guest-signal] ok   %s\n", what);
  }
}

static void check_u64(uint64_t got, uint64_t want, const char* what) {
  if (got != want)
    printf("[guest-signal]      got=%#llx want=%#llx\n",
           (unsigned long long)got, (unsigned long long)want);
  check(got == want, what);
}

static void mask_block_one(int sig) {
  sigset_t s;
  sigemptyset(&s);
  sigaddset(&s, sig);
  pthread_sigmask(SIG_SETMASK, &s, NULL);
}

static int mask_blocked(int sig) {
  sigset_t s;
  pthread_sigmask(SIG_SETMASK, NULL, &s);
  return sigismember(&s, sig) == 1;
}

static void mask_clear(void) {
  sigset_t s;
  sigemptyset(&s);
  pthread_sigmask(SIG_SETMASK, &s, NULL);
}

static void* g_fire;
static void* g_hits;
static void* g_invoke;

/* The guest fixture uses process-wide globals for the resume RIP/hits, so
 * concurrent fires are serialised; the mask assertions are still per-thread. */
static pthread_mutex_t g_fire_mu = PTHREAD_MUTEX_INITIALIZER;

static uint64_t fire_locked(uint64_t marker, uint64_t* hits) {
  uint64_t got;
  pthread_mutex_lock(&g_fire_mu);
  got = box64_call1(g_fire, marker);
  *hits = box64_call0(g_hits);
  pthread_mutex_unlock(&g_fire_mu);
  return got;
}

/* A real fire with a pre-set mask; asserts resume, no leaked handler bits and
 * that the pre-set bit survives. Returns the handler-hit count. */
static uint64_t probe_once(const char* tag, uint64_t marker) {
  uint64_t got, h;
  char buf[128];

  mask_block_one(SIGUSR2);
  snprintf(buf, sizeof(buf), "%s: pre-call mask blocks SIGUSR2", tag);
  check(mask_blocked(SIGUSR2), buf);

  got = fire_locked(marker, &h);

  snprintf(buf, sizeof(buf), "%s: handler ran", tag);
  check(h >= 1, buf);
  snprintf(buf, sizeof(buf), "%s: resumed after the signal (marker+hits)", tag);
  check_u64(got, marker + h, buf);
  snprintf(buf, sizeof(buf), "%s: pre-call bit SIGUSR2 survived", tag);
  check(mask_blocked(SIGUSR2), buf);
  snprintf(buf, sizeof(buf), "%s: sa_mask bit SIGALRM did not leak", tag);
  check(!mask_blocked(SIGALRM), buf);
  snprintf(buf, sizeof(buf), "%s: auto-blocked SIGUSR1 did not leak", tag);
  check(!mask_blocked(SIGUSR1), buf);
  snprintf(buf, sizeof(buf), "%s: SIGSEGV not blocked", tag);
  check(!mask_blocked(SIGSEGV), buf);
  mask_clear();
  return h;
}

/* Host bridge: re-enters the engine with the outer guest call still active. */
static uint64_t host_cb_fire(uint64_t a, uint64_t b, uint64_t c, uint64_t d,
                             uint64_t e, uint64_t f) {
  (void)b;
  (void)c;
  (void)d;
  (void)e;
  (void)f;
  return box64_call1(g_fire, a);
}

struct th_arg {
  int idx;
  int fail;
};

static void* th_probe(void* p) {
  struct th_arg* a = (struct th_arg*)p;
  uint64_t got, h;

  if (box64_ensure_emu() != 0) {
    a->fail = 1;
    return NULL;
  }
  mask_block_one(SIGUSR2);
  got = fire_locked(0x7000 + (uint64_t)a->idx, &h);
  if (got != 0x7000 + (uint64_t)a->idx + h) a->fail = 1;
  if (!mask_blocked(SIGUSR2)) a->fail = 1;   /* own pre-call bit kept */
  if (mask_blocked(SIGALRM)) a->fail = 1;    /* sa_mask bit did not leak */
  if (mask_blocked(SIGUSR1)) a->fail = 1;    /* auto-block did not leak */
  mask_clear();
  return NULL;
}

int main(int argc, char** argv) {
  void* lib;
  void* install_sym;
  void* cb;
  uint64_t h;
  box64_signal_stats_t st;
  int fast;

  if (argc < 2) {
    printf("usage: %s <sig_guest.so>\n", argv[0]);
    return 2;
  }
  box64_diag_set(0);
  if (box64_lib_init() != 0 || box64_ensure_emu() != 0) {
    const char* e = box64_dlerror(); /* read once: the slot is read-and-consume */
    printf("[guest-signal] engine init: %s\n", e ? e : "?");
    return 1;
  }
  fast = (box64_signal_policy_get() == BOX64_SIGNAL_POLICY_FAST);
  printf("[guest-signal] policy=%s\n", fast ? "fast" : "compat");

  lib = box64_dlopen(argv[1], RTLD_NOW);
  if (!lib) {
    const char* e = box64_dlerror();
    printf("[guest-signal] dlopen: %s\n", e ? e : "?");
    return 1;
  }
  install_sym = box64_dlsym(lib, "g_sig_install");
  g_fire = box64_dlsym(lib, "g_sig_fire");
  g_hits = box64_dlsym(lib, "g_sig_hits");
  g_invoke = box64_dlsym(lib, "g_sig_invoke");
  if (!install_sym || !g_fire || !g_hits || !g_invoke) {
    const char* e = box64_dlerror();
    printf("[guest-signal] missing guest symbols: %s\n", e ? e : "?");
    return 1;
  }

  box64_signal_stats_enable(1);
  check_u64(box64_call0(install_sym), 0,
            "guest rt_sigaction(SIGUSR1) installed (sa_mask=SIGALRM, no NODEFER)");
  if (g_fail) {
    /* Without a working rt_sigaction a fire would never terminate: stop here. */
    printf("[guest-signal] RESULT: FAIL (sigaction install failed; %d checks)\n",
           g_checks);
    return 1;
  }

  /* 1. basic real signal, twice, to catch residual leaks. */
  h = probe_once("probe", 0x600d);
  (void)h;
  probe_once("repeat", 0x600e);

  /* 2. nested: fire from a host bridge while an outer guest call is active. */
  cb = box64_callback((void*)host_cb_fire, "IIIIII");
  check(cb != NULL, "nested: callback bridge registered");
  if (cb) {
    uint64_t got, hh;
    mask_block_one(SIGUSR2);
    got = box64_call2(g_invoke, (uint64_t)(uintptr_t)cb, 0x600f);
    hh = box64_call0(g_hits);
    check_u64(got, 0x600f + hh, "nested: resumed across the outer call");
    check(mask_blocked(SIGUSR2), "nested: pre-call bit survived");
    check(!mask_blocked(SIGALRM) && !mask_blocked(SIGUSR1),
          "nested: no handler-added bits leaked");
    mask_clear();
  }

  /* 3. worker threads: each on its own emu with its own mask. */
  {
    enum { NTH = 3 };
    pthread_t th[NTH];
    struct th_arg ta[NTH];
    int i, spawned = 0, bad = 0;
    for (i = 0; i < NTH; ++i) {
      ta[i].idx = i;
      ta[i].fail = 0;
      if (pthread_create(&th[i], NULL, th_probe, &ta[i]) == 0)
        spawned++;
      else
        break;
    }
    check(spawned == NTH, "threads: all workers spawned");
    for (i = 0; i < spawned; ++i) pthread_join(th[i], NULL);
    for (i = 0; i < spawned; ++i) bad += ta[i].fail;
    check(bad == 0, "threads: per-thread mask + no leak on every emu");
  }

  memset(&st, 0, sizeof(st));
  box64_signal_stats(&st);
  if (fast) {
    check(st.setjmp_skips > 0, "FAST: jmpbuf armed with savemask=0");
    check(st.setjmp_saves == 0, "FAST: no per-call mask save");
    check(st.mask_restores > 0, "FAST: slow-path mask restore really ran");
  } else {
    check(st.setjmp_saves > 0, "COMPAT: jmpbuf armed with savemask=1");
    check(st.setjmp_skips == 0, "COMPAT: no savemask=0 arming");
    check(st.mask_restores == 0, "COMPAT: no slow-path restore (siglongjmp)");
  }

  box64_lib_fini();

  if (g_fail) {
    printf("[guest-signal] RESULT: FAIL (%d checks)\n", g_checks);
    return 1;
  }
  printf("[guest-signal] RESULT: PASS (%d checks)\n", g_checks);
  return 0;
}
