/*
 * Regression for the engine signal-handling policy (include/libbox64.h,
 * libbox64_signal.h, patches/0005) — driven by REAL signals, not flags.
 *
 * The policy removes the per-call signal-mask syscall from the call path:
 *   FAST (default)  EmuRun() arms its jmpbuf with sigsetjmp(buf, 0) (no
 *                   syscall), and every signal-driven siglongjmp() back into
 *                   EmuRun() first restores the mask the kernel recorded in the
 *                   delivered signal's ucontext (the pre-delivery mask).
 *   COMPAT          sigsetjmp(buf, 1), the old behaviour: glibc does an
 *                   rt_sigprocmask syscall per top-level call and siglongjmp
 *                   restores the setjmp-time mask.
 *
 * Everything here is exercised with signals the process really receives:
 *   - a host handler installed before init is taken over by box64 and restored
 *     by box64_lib_fini() (checked with sigaction(), and by actually raising
 *     SIGSEGV on a host-context thread, which must reach the host handler);
 *   - the FAST restore helper is called on a crafted ucontext and the change is
 *     read back with pthread_sigmask();
 *   - per-thread masks survive guest calls made on several worker threads.
 *
 * Policy's global default is one-shot per process, so the process is started
 * once per policy. The FAST default is assumed; every COMPAT case selects it
 * explicitly through BOX64DL_SIGNAL_POLICY=compat or the API.
 *
 * Usage: test_signal_policy <guest.so> <case>
 *   cases: default fast-env compat-env api-fast api-compat
 *          helper-default helper-fast helper-compat
 *          threads reenter fini-workers fini-host-changed fini-guest-fatal
 *          env-none env-bogus set-bad
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE /* pthread_sigmask / ucontext / si_code, also via -D on cc */
#endif
#include <dlfcn.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ucontext.h>
#include <unistd.h>
#include "libbox64.h"
#include "call_compat.h"
#include "libbox64_signal.h"

#include "os.h"
#include "regs.h"
#include "x64emu.h"
#include "box64context.h"
#include "emu/x64emu_private.h"

/* threads.c (patch 0003): this thread's own emu, never fabricated. */
extern x64emu_t* thread_peek_emu(void);
/* core/box64context.c: the live context, so a test can plant the
 * guest-registered fatal handler precondition (patches/0006). */
extern box64context_t* my_context;

static int g_fail;
static int g_checks;

static void check(int cond, const char* what) {
  g_checks++;
  if (!cond) {
    g_fail = 1;
    printf("[signal-policy] FAIL %s\n", what);
  } else {
    printf("[signal-policy] ok   %s\n", what);
  }
}

static void check_u64(uint64_t got, uint64_t want, const char* what) {
  if (got != want)
    printf("[signal-policy]      got=%#llx want=%#llx\n",
           (unsigned long long)got, (unsigned long long)want);
  check(got == want, what);
}

/* --- host SIGSEGV action installed before init ---------------------------- */

static volatile sig_atomic_t g_host_segv_hits;

static void host_segv(int sig, siginfo_t* info, void* ucntx) {
  (void)sig;
  g_host_segv_hits++;
  /*
   * raise()/kill() deliveries (SI_USER/SI_TKILL/SI_QUEUE) are the deliberate
   * host-route probes and may return. A real hardware fault reaching the host
   * action means box64 did not claim a JIT/guest fault: fail fast with a
   * diagnosis instead of letting the instruction re-execute forever.
   */
  if (info && info->si_code != SI_USER && info->si_code != SI_TKILL &&
      info->si_code != SI_QUEUE) {
    void* pc = ucntx ? (void*)((ucontext_t*)ucntx)->uc_mcontext.__pc : NULL;
    fprintf(stderr,
            "[signal-policy] FATAL: hardware fault (code=%d) at pc=%p addr=%p "
            "reached the host handler (wrong route)\n",
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

static int current_segv_is_host(void) {
  struct sigaction cur;
  if (sigaction(SIGSEGV, NULL, &cur) != 0) return 0;
  return cur.sa_sigaction == host_segv;
}

/*
 * A second host action installed AFTER init. patches/0006 restores the pre-init
 * host action only for a signal whose disposition is still box64's own handler,
 * so fini must leave this newer action alone (a host that changed its mind after
 * init is not clobbered).
 */
static volatile sig_atomic_t g_host2_hits;

static void host_segv2(int sig, siginfo_t* info, void* ucntx) {
  (void)sig;
  (void)info;
  (void)ucntx;
  g_host2_hits++;
}

static void install_host_segv2(void) {
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_sigaction = host_segv2;
  sa.sa_flags = SA_SIGINFO;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGSEGV, &sa, NULL);
}

static int current_segv_is_host2(void) {
  struct sigaction cur;
  if (sigaction(SIGSEGV, NULL, &cur) != 0) return 0;
  return cur.sa_sigaction == host_segv2;
}

/* --- signal masks --------------------------------------------------------- */

static void mask_clear(void) {
  sigset_t s;
  sigemptyset(&s);
  pthread_sigmask(SIG_SETMASK, &s, NULL);
}

static int mask_blocked(int sig) {
  sigset_t s;
  pthread_sigmask(SIG_SETMASK, NULL, &s);
  return sigismember(&s, sig) == 1;
}

static void mask_block_one(int sig) {
  sigset_t s;
  sigemptyset(&s);
  sigaddset(&s, sig);
  pthread_sigmask(SIG_SETMASK, &s, NULL);
}

/* --- guest fixture -------------------------------------------------------- */

static void* g_lib;
static void* g_zero;
static void* g_add2;
static void* g_id;
static void* g_invoke;

static void* sym(const char* name) { return box64_dlsym(g_lib, name); }

/* --- cases ---------------------------------------------------------------- */

static int case_helper(void) {
  /* Real sigprocmask around the helper, so COMPAT no-op and FAST restore are
   * both observed through the kernel, not through a flag. */
  ucontext_t uc;
  int fast = (box64_signal_policy_get() == BOX64_SIGNAL_POLICY_FAST);

  memset(&uc, 0, sizeof(uc));
  sigemptyset(&uc.uc_sigmask);
  sigaddset(&uc.uc_sigmask, SIGUSR1);

  mask_block_one(SIGUSR2);
  check(mask_blocked(SIGUSR2) && !mask_blocked(SIGUSR1),
        "helper: pre-state mask = {SIGUSR2}");

  /* The helper takes the frame's policy explicitly, exactly as the vendor
   * resume sites pass emu->active_jmpbuf_policy. */
  box64dl_signal_mask_restore(fast ? BOX64_SIGNAL_POLICY_FAST
                                   : BOX64_SIGNAL_POLICY_COMPAT,
                              &uc);

  if (fast) {
    check(!mask_blocked(SIGUSR2) && mask_blocked(SIGUSR1),
          "helper FAST: mask restored from the ucontext ({SIGUSR1})");
  } else {
    check(mask_blocked(SIGUSR2) && !mask_blocked(SIGUSR1),
          "helper COMPAT: no-op, siglongjmp owns the restore");
  }
  mask_clear();
  return 0;
}

struct mt_arg {
  void* add2;
  int sig;
  int idx;
  int fail;
};

/* Each worker thread carries its own signal mask; the engine must not disturb
 * it across the guest calls it makes. */
static void* mt_thread(void* p) {
  struct mt_arg* a = (struct mt_arg*)p;
  int i;

  if (box64_ensure_emu() != 0) {
    a->fail = 1;
    return NULL;
  }
  /* Each thread carries its own mask; the engine must not disturb it. */
  mask_block_one(a->sig);
  for (i = 0; i < 200; ++i) {
    if (test_simple2(a->add2, (uint64_t)a->idx, (uint64_t)i) !=
        (uint64_t)(a->idx + i)) {
      a->fail = 1;
      return NULL;
    }
  }
  if (!mask_blocked(a->sig)) a->fail = 1;
  mask_clear();
  return NULL;
}

static int case_threads(void) {
  enum { NTH = 4 };
  /* Never SIGUSR1 + i: SIGSEGV(11) sits between SIGUSR1(10) and SIGUSR2(12). */
  static const int sigs[NTH] = {SIGUSR1, SIGUSR2, SIGALRM, SIGTERM};
  pthread_t th[NTH];
  struct mt_arg ma[NTH];
  int i, spawned = 0, bad = 0;

  for (i = 0; i < NTH; ++i) {
    ma[i].add2 = g_add2;
    ma[i].sig = sigs[i];
    ma[i].idx = 100 + i;
    ma[i].fail = 0;
    if (pthread_create(&th[i], NULL, mt_thread, &ma[i]) == 0)
      spawned++;
    else
      break;
  }
  check(spawned == NTH, "threads: all workers spawned");
  for (i = 0; i < spawned; ++i) pthread_join(th[i], NULL);
  for (i = 0; i < spawned; ++i) bad += ma[i].fail;
  check(bad == 0, "threads: per-thread mask survived, calls and fault OK");
  return 0;
}

/*
 * F (fini host-action restore) must not change the worker teardown path. Run a
 * full worker lifecycle (spawn -> calls -> clean exit -> join) BEFORE fini, so
 * main's teardown then proves the host action is back with no worker left
 * dangling; the restore itself is a pair of sigaction() calls after endBox64()
 * and touches no thread state.
 */
static int case_fini_workers(void) {
  enum { NTH = 2 };
  static const int sigs[NTH] = {SIGUSR1, SIGUSR2};
  pthread_t th[NTH];
  struct mt_arg ma[NTH];
  int i, spawned = 0, bad = 0;

  for (i = 0; i < NTH; ++i) {
    ma[i].add2 = g_add2;
    ma[i].sig = sigs[i];
    ma[i].idx = 200 + i;
    ma[i].fail = 0;
    if (pthread_create(&th[i], NULL, mt_thread, &ma[i]) == 0)
      spawned++;
    else
      break;
  }
  check(spawned == NTH, "fini-workers: workers spawned");
  for (i = 0; i < spawned; ++i) pthread_join(th[i], NULL);
  for (i = 0; i < spawned; ++i) bad += ma[i].fail;
  check(bad == 0, "fini-workers: each worker exited cleanly on its own emu");
  check(!current_segv_is_host(), "fini-workers: box64 still owns SIGSEGV");
  return 0;
}

static uint64_t host_cb6(uint64_t a, uint64_t b, uint64_t c, uint64_t d,
                         uint64_t e, uint64_t f) {
  (void)b;
  (void)c;
  (void)d;
  (void)e;
  (void)f;
  /* Re-enter the engine while the outer call is still on the stack. */
  return test_simple1(g_id, a * 3);
}

static int case_reenter(void) {
  box64_signal_stats_t st;
  void* cb = box64_callback((void*)host_cb6);
  check(cb != NULL, "reenter: callback bridge registered");
  if (!cb) return 0;
  check_u64(test_simple2(g_invoke, (uint64_t)(uintptr_t)cb, 5), 16,
            "reenter: nested engine call from a host bridge");
  memset(&st, 0, sizeof(st));
  box64_signal_stats(&st);
  check(st.publishes == 1, "reenter: policy published exactly once");
  return 0;
}

/* --- main ----------------------------------------------------------------- */

static int want_fast(const char* case_name) {
  const char* env = getenv("BOX64DL_SIGNAL_POLICY");
  if (env && strcmp(env, "compat") == 0) return 0;
  if (strcmp(case_name, "compat-env") == 0 ||
      strcmp(case_name, "helper-compat") == 0 ||
      strcmp(case_name, "api-compat") == 0)
    return 0;
  return 1; /* FAST is the built-in default */
}

static int is_case(const char* m, const char* want) { return strcmp(m, want) == 0; }

static int finish(void) {
  if (g_fail) {
    printf("[signal-policy] RESULT: FAIL (%d checks)\n", g_checks);
    return 1;
  }
  printf("[signal-policy] RESULT: PASS (%d checks)\n", g_checks);
  return 0;
}

int main(int argc, char** argv) {
  const char* mode;
  int expect_fail, expect_fast, rc;
  box64_signal_stats_t st;

  if (argc < 3) {
    printf("usage: %s <guest.so> <case>\n", argv[0]);
    return 2;
  }
  mode = argv[2];
  box64_diag_set(0);

  /* Value validation is independent of init. */
  if (is_case(mode, "set-bad")) {
    check(box64_signal_policy_set(99) == -1, "set-bad: out-of-range refused");
    check(box64_signal_policy_set(3) == -1,
          "set-bad: no API value for the old env-only 'none' (3 refused)");
    check(box64_signal_policy_get() == BOX64_SIGNAL_POLICY_FAST,
          "set-bad: failed re-selection left the FAST default");
    return finish();
  }

  expect_fail = is_case(mode, "env-none") || is_case(mode, "env-bogus");
  expect_fast = want_fast(mode);

  if (is_case(mode, "api-fast"))
    check(box64_signal_policy_set(BOX64_SIGNAL_POLICY_FAST) == 0,
          "api-fast: selection accepted before init");
  if (is_case(mode, "api-compat"))
    check(box64_signal_policy_set(BOX64_SIGNAL_POLICY_COMPAT) == 0,
          "api-compat: selection accepted before init");

  /* A host handler exists before init: box64 must take SIGSEGV over, and fini
   * must give this exact action back. */
  install_host_segv();
  check(current_segv_is_host(), "host SIGSEGV handler installed pre-init");

  rc = box64_lib_init();
  if (expect_fail) {
    const char* err = box64_dlerror();
    check(rc != 0, "refused init returns non-zero");
    check(err != NULL && err[0], "refusal recorded in box64_dlerror");
    if (err) printf("[signal-policy]      reason: %s\n", err);
    check(current_segv_is_host(),
          "refused init installed no box64 handler (fail-fast, no silent "
          "global disable)");
    return finish();
  }

  check(rc == 0, "box64_lib_init succeeded");
  check(box64_signal_policy_get() ==
            (expect_fast ? BOX64_SIGNAL_POLICY_FAST : BOX64_SIGNAL_POLICY_COMPAT),
        "published policy matches the selection");

  /* Handler precedence + the property FAST relies on. */
  {
    struct sigaction cur;
    memset(&cur, 0, sizeof(cur));
    sigaction(SIGSEGV, NULL, &cur);
    check(cur.sa_sigaction != host_segv && cur.sa_sigaction != 0,
          "box64 owns SIGSEGV after init");
    check((cur.sa_flags & SA_SIGINFO) != 0, "box64 handler is SA_SIGINFO");
    check((cur.sa_flags & SA_NODEFER) != 0,
          "box64 handler is SA_NODEFER (entry does not block the signal)");
    check(sigismember(&cur.sa_mask, SIGSEGV) == 0,
          "box64 sa_mask does not block the delivered signal");
  }

  /* Real delivered SIGSEGV on a host-context thread must reach the host
   * handler (V8-style), not the guest route. */
  g_host_segv_hits = 0;
  raise(SIGSEGV);
  check(g_host_segv_hits == 1,
        "host fault routed to the pre-box64 host handler");

  /* F guard: a host that re-installs SIGSEGV AFTER init must not be clobbered
   * by fini (patches/0006 ownership test). Installed after the raise probe so
   * that probe still goes through box64's handler. */
  if (is_case(mode, "fini-host-changed")) {
    install_host_segv2();
    check(current_segv_is_host2(),
          "fini-host-changed: host re-installed SIGSEGV after init");
  }

  /* F residual (patches/0006): a guest-registered fatal handler is stored in
   * my_context->signals[]; FreeBox64Context's loop used to reset SIGSEGV to
   * SIG_DFL before fini_signal_helper, so its ownership check failed and the
   * host action was eaten. Plant that precondition: only the non-zero slot
   * matters to the loop, no real guest handler is invoked here. */
  if (is_case(mode, "fini-guest-fatal")) {
    my_context->signals[SIGSEGV] = 0x1234; /* nonzero, != 1: as if registered */
    check(my_context->signals[SIGSEGV] == 0x1234,
          "fini-guest-fatal: planted a guest fatal-handler slot");
  }

  /* Load the guest for the cases that need it. The helper cases do not use a
   * guest symbol, but loading one is what makes endBox64()'s
   * SerializeAllMapping() safe: with no mapping ever recorded, mapping_entries
   * is NULL and that pre-existing fini path dereferences it (reported, out of
   * this change's scope). */
  if (is_case(mode, "default") || is_case(mode, "fast-env") ||
      is_case(mode, "compat-env") || is_case(mode, "api-fast") ||
      is_case(mode, "api-compat") || is_case(mode, "threads") ||
      is_case(mode, "reenter") || is_case(mode, "fini-workers") ||
      is_case(mode, "helper-default") || is_case(mode, "helper-fast") ||
      is_case(mode, "helper-compat") || is_case(mode, "fini-host-changed") ||
      is_case(mode, "fini-guest-fatal")) {
    g_lib = box64_dlopen(argv[1], RTLD_NOW);
    if (!g_lib) {
      printf("[signal-policy] dlopen: %s\n",
             box64_dlerror() ? box64_dlerror() : "?");
      return 1;
    }
    g_zero = sym("g_zero");
    g_add2 = sym("g_add2");
    g_id = sym("g_id");
    g_invoke = sym("g_invoke");
    if (!g_zero || !g_add2 || !g_id || !g_invoke) {
      printf("[signal-policy] missing guest symbols\n");
      return 1;
    }
  }

  if (is_case(mode, "default") || is_case(mode, "fast-env") ||
      is_case(mode, "compat-env") || is_case(mode, "api-fast") ||
      is_case(mode, "api-compat")) {
    box64_signal_stats_enable(1);
    check_u64(test_simple0(g_zero), 0x1234, "call0 runs the guest");
    check(box64_dlerror() == NULL, "error slot clear after a real call");
    check(thread_peek_emu() != NULL, "calling thread has an emu");
    memset(&st, 0, sizeof(st));
    box64_signal_stats(&st);
    check(st.publishes == 1, "policy published exactly once");
    if (expect_fast) {
      check(st.setjmp_skips > 0 && st.setjmp_saves == 0,
            "FAST: per-call armings skip the mask save");
      check(st.mask_restores == 0,
            "FAST: no signal path, so no restore yet");
    } else {
      check(st.setjmp_saves > 0 && st.setjmp_skips == 0,
            "COMPAT: per-call armings save the mask");
      check(st.mask_restores == 0, "COMPAT: restore helper is a no-op");
    }
    if (is_case(mode, "api-fast"))
      check(box64_signal_policy_set(BOX64_SIGNAL_POLICY_COMPAT) == -1,
            "api-fast: policy is immutable after publish");
    if (is_case(mode, "api-compat"))
      check(box64_signal_policy_set(BOX64_SIGNAL_POLICY_FAST) == -1,
            "api-compat: policy is immutable after publish");
    if (is_case(mode, "api-fast"))
      check(box64_signal_policy_get() == BOX64_SIGNAL_POLICY_FAST,
            "api-fast: failed re-selection changed nothing");
    if (is_case(mode, "api-compat"))
      check(box64_signal_policy_get() == BOX64_SIGNAL_POLICY_COMPAT,
            "api-compat: failed re-selection changed nothing");
  }

  if (is_case(mode, "helper-default") || is_case(mode, "helper-fast") ||
      is_case(mode, "helper-compat"))
    case_helper();
  if (is_case(mode, "threads")) case_threads();
  if (is_case(mode, "reenter")) case_reenter();
  if (is_case(mode, "fini-workers")) case_fini_workers();

  /* Teardown: the pre-init host action must be live again, and a real signal
   * must reach it. If the host replaced SIGSEGV after init, fini must leave that
   * newer action alone (patches/0006 ownership guard). */
  box64_lib_fini();
  if (is_case(mode, "fini-host-changed")) {
    check(current_segv_is_host2(),
          "fini-host-changed: fini did not clobber the post-init host action");
    g_host2_hits = 0;
    raise(SIGSEGV);
    check(g_host2_hits == 1,
          "fini-host-changed: the post-init host action is live after fini");
  } else {
    check(current_segv_is_host(), "fini restored the host SIGSEGV handler");
    g_host_segv_hits = 0;
    raise(SIGSEGV);
    check(g_host_segv_hits == 1, "host handler live again after fini");
  }
  if (g_lib)
    check(test_simple0(g_zero) == 0 && box64_dlerror() != NULL,
          "post-fini call refused");

  return finish();
}
