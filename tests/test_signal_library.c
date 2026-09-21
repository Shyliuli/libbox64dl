/*
 * Regression for the PER-LIBRARY signal policy (include/libbox64.h,
 * src/signal_policy.c, patches/0005).
 *
 * The engine resolves a call's mask policy from the library that owns the entry
 * symbol, with priority explicit per-handle override > environment library rule
 * > global default (FAST). This test drives that selection through the public
 * API and the environment, using two distinct guest .so images so "per library"
 * is observable rather than assumed.
 *
 * Cases:
 *   precedence        explicit override > env rule > default
 *   env-basename      BOX64DL_SIGNAL_LIBRARY_RULES basename match
 *   env-fullpath      BOX64DL_SIGNAL_LIBRARY_RULES exact path match
 *   env-nosubstring   'foo.so' must not match 'libfoo.so'
 *   badrule           a malformed rule fails box64_lib_init()
 *   constructors      an env rule applies to the target library's constructor
 *                     (observed through the per-thread setjmp arm counters)
 *   mixed             REAL SIGUSR1 inside a nested inner library call, with the
 *                     inner policy opposite to the outer one: the mask restored
 *                     after the signal-driven siglongjmp must follow the INNER
 *                     frame's policy (FAST => pre-delivery M1, COMPAT => entry
 *                     M0), proving the mixed-mode forced new frame. Runs both
 *                     directions in one process.
 *   invalid-handle    bogus/NULL handle refused
 *   invalid-config    out-of-range policy refused
 *   unload-reload     a closed library's policy does not leak to the reload
 *   threads           two worker threads on two libraries keep their own scope
 *
 * Usage: test_signal_library <libA.so> <libB.so> <case>
 *   libA is the outer/invoke library (tests/guest/call_guest.c), libB the
 *   real-SIGUSR1 fixture (tests/guest/sig_guest2.c, renamed exports).
 *
 * The environment cases are driven by BOX64DL_SIGNAL_LIBRARY_RULES from the
 * Makefile; parsing happens at init, so the variable must already be set.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <dlfcn.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "libbox64.h"
#include "libbox64_ext.h"    /* foreign-loader image-policy seam */
#include "libbox64_signal.h" /* internal: capacity bound unit check */

static int g_fail;
static int g_checks;

static void check(int cond, const char* what) {
  g_checks++;
  if (!cond) {
    g_fail = 1;
    printf("[signal-library] FAIL %s\n", what);
  } else {
    printf("[signal-library] ok   %s\n", what);
  }
}

static void check_policy(int got, int want, const char* what) {
  if (got != want)
    printf("[signal-library]      got=%d want=%d\n", got, want);
  check(got == want, what);
}

static int is_case(const char* m, const char* want) { return strcmp(m, want) == 0; }

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

/* --- mixed-mode nested real-signal probe ---------------------------------- */

static void* g_inner_fire; /* g_sig_change_then_fire2 in lib B */
static uint64_t g_inner_marker;
static uint64_t g_inner_m1;

/* Host bridge run by lib A's g_invoke: re-enters the engine targeting lib B,
 * which sets its mask and raises SIGUSR1. The signal-driven siglongjmp resumes
 * lib B's EmuRun (a forced new frame when the policies differ). */
static uint64_t host_cb_mixed(uint64_t a, uint64_t b, uint64_t c, uint64_t d,
                              uint64_t e, uint64_t f) {
  (void)a;
  (void)b;
  (void)c;
  (void)d;
  (void)e;
  (void)f;
  return box64_call2(g_inner_fire, g_inner_marker, g_inner_m1);
}

static int mixed_run(void* A, void* B, void* invoke_a, void* fire_b,
                     void* hits_b, int outer_fast, int inner_fast,
                     const char* tag) {
  void* cb;
  uint64_t before, after;
  const uint64_t m1 = 1UL << (SIGALRM - 1); /* pre-delivery mask M1 */

  printf("[signal-library] -- %s: outer=%s inner=%s\n", tag,
         outer_fast ? "fast" : "compat", inner_fast ? "fast" : "compat");

  check(box64_signal_policy_set_library(
            A, outer_fast ? BOX64_SIGNAL_POLICY_FAST : BOX64_SIGNAL_POLICY_COMPAT) == 0,
        "mixed: outer policy set");
  check(box64_signal_policy_set_library(
            B, inner_fast ? BOX64_SIGNAL_POLICY_FAST : BOX64_SIGNAL_POLICY_COMPAT) == 0,
        "mixed: inner policy set");
  cb = box64_callback((void*)host_cb_mixed, "IIIIII");
  check(cb != NULL, "mixed: nested callback bridge registered");
  if (!cb) return 0;

  g_inner_fire = fire_b;
  g_inner_marker = 0x9000 + (uint64_t)inner_fast;
  g_inner_m1 = m1;

  mask_block_one(SIGUSR2); /* M0 */
  before = box64_call0(hits_b);
  (void)box64_call2(invoke_a, (uint64_t)(uintptr_t)cb, 0);
  after = box64_call0(hits_b);
  check(after > before, "mixed: real SIGUSR1 handler ran in the inner scope");

  if (inner_fast) {
    check(mask_blocked(SIGALRM) && !mask_blocked(SIGUSR2),
          "mixed: inner FAST kept the pre-delivery mask M1");
    check(!mask_blocked(SIGUSR1) && !mask_blocked(SIGSEGV),
          "mixed: inner FAST leaked no handler-added bits");
  } else {
    check(mask_blocked(SIGUSR2) && !mask_blocked(SIGALRM),
          "mixed: inner COMPAT restored the entry mask M0");
    check(!mask_blocked(SIGUSR1) && !mask_blocked(SIGSEGV),
          "mixed: inner COMPAT leaked no handler-added bits");
  }
  mask_clear();
  return 0;
}

/* --- cases ---------------------------------------------------------------- */

static int case_precedence(void* A, void* B) {
  const char* env = getenv("BOX64DL_SIGNAL_LIBRARY_RULES");
  check(env != NULL && strstr(env, "libcall_guest.so=compat") != NULL,
        "precedence: env rule selected A=compat");
  check_policy(box64_signal_policy_get_library(A), BOX64_SIGNAL_POLICY_COMPAT,
               "precedence: env rule applied to A");
  check_policy(box64_signal_policy_get_library(B), BOX64_SIGNAL_POLICY_FAST,
               "precedence: B keeps the FAST default");
  check(box64_signal_policy_set_library(A, BOX64_SIGNAL_POLICY_FAST) == 0,
        "precedence: explicit FAST override accepted");
  check_policy(box64_signal_policy_get_library(A), BOX64_SIGNAL_POLICY_FAST,
               "precedence: explicit beats the env rule");
  check_policy(box64_signal_policy_get_library(B), BOX64_SIGNAL_POLICY_FAST,
               "precedence: the override did not leak to B");
  return 0;
}

static int case_env(void* A, void* B, const char* mode) {
  int wantA = BOX64_SIGNAL_POLICY_FAST, wantB = BOX64_SIGNAL_POLICY_FAST;
  if (is_case(mode, "env-basename")) {
    wantB = BOX64_SIGNAL_POLICY_COMPAT; /* libsig_guest2.so=compat */
  } else if (is_case(mode, "env-fullpath")) {
    wantA = BOX64_SIGNAL_POLICY_COMPAT; /* <libA path>=compat */
  } /* env-nosubstring: 'call_guest.so' must match neither */
  check_policy(box64_signal_policy_get_library(A), wantA, "env: A effective policy");
  check_policy(box64_signal_policy_get_library(B), wantB, "env: B effective policy");
  check(box64_signal_policy_set_library(B, BOX64_SIGNAL_POLICY_COMPAT) == 0,
        "env: explicit override on B accepted");
  check_policy(box64_signal_policy_get_library(B), BOX64_SIGNAL_POLICY_COMPAT,
               "env: explicit override beats the rule");
  return 0;
}

struct thr_arg {
  void* sym;
  uint64_t want;
  int expect_fast;
  int fail;
};

static void* thr_call(void* p) {
  struct thr_arg* a = (struct thr_arg*)p;
  box64_signal_stats_t st;
  if (box64_ensure_emu() != 0 || box64_call0(a->sym) != a->want) {
    a->fail = 1;
    return NULL;
  }
  /* Per-thread arm counters prove the worker actually ran under ITS library's
   * policy: a FAST arm increments skips, a COMPAT arm saves. Without this the
   * case could pass with every worker on the same policy. */
  memset(&st, 0, sizeof(st));
  box64_signal_stats(&st);
  if (a->expect_fast) {
    if (!(st.setjmp_skips > 0 && st.setjmp_saves == 0)) a->fail = 1;
  } else {
    if (!(st.setjmp_saves > 0 && st.setjmp_skips == 0)) a->fail = 1;
  }
  return NULL;
}

static int case_threads(void* A, void* B, void* symA, void* symB) {
  pthread_t t1, t2;
  struct thr_arg a1, a2;
  check(box64_signal_policy_set_library(A, BOX64_SIGNAL_POLICY_COMPAT) == 0,
        "threads: A set COMPAT");
  check(box64_signal_policy_set_library(B, BOX64_SIGNAL_POLICY_FAST) == 0,
        "threads: B set FAST");
  box64_signal_stats_enable(1); /* shared global; set before spawning */
  a1.sym = symA;
  a1.want = 0x1234; /* g_zero */
  a1.expect_fast = 0;
  a1.fail = 0;
  a2.sym = symB;
  a2.want = 0; /* g_sig_hits2 before any signal */
  a2.expect_fast = 1;
  a2.fail = 0;
  if (pthread_create(&t1, NULL, thr_call, &a1) != 0 ||
      pthread_create(&t2, NULL, thr_call, &a2) != 0) {
    check(0, "threads: worker threads spawned");
    return 0;
  }
  pthread_join(t1, NULL);
  pthread_join(t2, NULL);
  check(!a1.fail, "threads: worker on A ran COMPAT (per-thread arm counters)");
  check(!a2.fail, "threads: worker on B ran FAST (per-thread arm counters)");
  return 0;
}

/* --- main ----------------------------------------------------------------- */

static int finish(void) {
  if (g_fail) {
    printf("[signal-library] RESULT: FAIL (%d checks)\n", g_checks);
    return 1;
  }
  printf("[signal-library] RESULT: PASS (%d checks)\n", g_checks);
  return 0;
}

int main(int argc, char** argv) {
  const char* mode;
  const char* dep_path = NULL;
  void *A, *B;

  if (argc < 4) {
    printf("usage: %s <libA.so> <libB.so> <case> [dep.so]\n", argv[0]);
    return 2;
  }
  mode = argv[3];
  if (argc > 4) dep_path = argv[4];
  box64_diag_set(0);

  /* A malformed rules variable must fail init before anything else. */
  if (is_case(mode, "badrule")) {
    int rc = box64_lib_init();
    const char* e = box64_dlerror();
    check(rc != 0, "badrule: box64_lib_init refused");
    check(e != NULL && e[0], "badrule: refusal recorded in box64_dlerror");
    if (e) printf("[signal-library]      reason: %s\n", e);
    return finish();
  }

  /* Dependency unload: open an image with DT_NEEDED on libB, then close it. B's
   * policy range must be dropped at the real unmap point (FreeElfMemory), not
   * only for a top-level box64_dlclose handle. The live-override count is the
   * observable (re-opening B would re-apply the rule and hide a leak). Handled
   * before the generic dlopen so the test holds no B ref. */
  if (is_case(mode, "dep-unload")) {
    void* dep;
    int before, during, after;
    if (!dep_path) {
      check(0, "dep-unload: dep path argument required");
      box64_lib_fini();
      return finish();
    }
    before = box64dl_signal_lib_rules_active();
    dep = box64_dlopen(dep_path, RTLD_NOW);
    check(dep != NULL, "dep-unload: dependency image loaded (needs libB)");
    during = box64dl_signal_lib_rules_active();
    check(during > before,
          "dep-unload: the dependency's policy range was registered");
    if (dep) box64_dlclose(dep);
    after = box64dl_signal_lib_rules_active();
    check(after == before,
          "dep-unload: dependency range cleared at image free (no residual)");
    /* endBox64() walks my_context->elfs[0] (pre-existing path); keep an image
     * mapped so teardown is valid now that the dependency is gone. */
    A = box64_dlopen(argv[1], RTLD_NOW);
    check(A != NULL, "dep-unload: libA re-opened for a valid teardown");
    box64_lib_fini();
    return finish();
  }

  if (box64_lib_init() != 0) {
    const char* e = box64_dlerror(); /* read once: the slot is read-and-consume */
    printf("[signal-library] init: %s\n", e ? e : "?");
    return 1;
  }

  /* The constructor runs during dlopen, so its arm counters must be read
   * around the open, before the generic dlopen below. */
  if (is_case(mode, "constructors")) {
    const char* rules = getenv("BOX64DL_SIGNAL_LIBRARY_RULES");
    int want_fast = !(rules && strstr(rules, "libcall_guest.so=compat"));
    box64_signal_stats_t before, after;
    box64_signal_stats_enable(1);
    memset(&before, 0, sizeof(before));
    box64_signal_stats(&before);
    A = box64_dlopen(argv[1], RTLD_NOW);
    check(A != NULL, "constructors: libA loaded");
    memset(&after, 0, sizeof(after));
    box64_signal_stats(&after);
    if (want_fast) {
      check(after.setjmp_skips > before.setjmp_skips &&
                after.setjmp_saves == before.setjmp_saves,
            "constructors: env rule armed the constructor frame FAST");
    } else {
      check(after.setjmp_saves > before.setjmp_saves &&
                after.setjmp_skips == before.setjmp_skips,
            "constructors: env rule armed the constructor frame COMPAT");
    }
    box64_lib_fini();
    return finish();
  }

  if (is_case(mode, "overflow")) {
    int i, ok = 1;
    A = box64_dlopen(argv[1], RTLD_NOW);
    check(A != NULL, "overflow: library loaded");
    for (i = 0; i < 64; ++i) {
      uintptr_t lo = (uintptr_t)0x100000000ull + (uintptr_t)i * 0x1000ull;
      if (box64dl_signal_lib_range_set(lo, lo + 0x1000ull,
                                       BOX64_SIGNAL_POLICY_COMPAT) != 0)
        ok = 0;
    }
    check(ok, "overflow: first 64 overrides accepted");
    check(box64dl_signal_lib_range_set((uintptr_t)0x200000000ull,
                                       (uintptr_t)0x200000100ull,
                                       BOX64_SIGNAL_POLICY_COMPAT) == -1,
          "overflow: 65th override refused, never a silent FAST fallback");
    box64_lib_fini();
    return finish();
  }

  if (is_case(mode, "foreign")) {
    const uintptr_t base = (uintptr_t)0x300000000ull;
    A = box64_dlopen(argv[1], RTLD_NOW);
    check(A != NULL, "foreign: library loaded (valid teardown)");
    check(box64_signal_policy_set_range(base, 0x10000,
                                        BOX64_SIGNAL_POLICY_COMPAT) == 0,
          "foreign: set_range(base,size) accepted");
    check(box64_signal_policy_get_address(base) == BOX64_SIGNAL_POLICY_COMPAT &&
              box64_signal_policy_get_address(base + 0x8000) ==
                  BOX64_SIGNAL_POLICY_COMPAT,
          "foreign: effective policy resolves inside the registered image");
    box64_signal_policy_clear_range(base);
    check(box64_signal_policy_get_address(base) == BOX64_SIGNAL_POLICY_FAST,
          "foreign: address falls back to the global default after clear");
    check(box64_signal_policy_set_range(0, 0x1000,
                                        BOX64_SIGNAL_POLICY_COMPAT) == -1 &&
              box64_signal_policy_set_range((uintptr_t)-0x100, 0x200,
                                            BOX64_SIGNAL_POLICY_COMPAT) == -1,
          "foreign: NULL / overflowing range refused");
    check(box64_signal_policy_rule_for_name("libsig_guest2.so") ==
              BOX64_SIGNAL_POLICY_COMPAT,
          "foreign: rule_for_name matches the env rule");
    check(box64_signal_policy_rule_for_name("no_such_lib.so") == -1,
          "foreign: rule_for_name returns -1 for no match");
    box64_lib_fini();
    return finish();
  }

  if (is_case(mode, "invalid-config")) {
    A = box64_dlopen(argv[1], RTLD_NOW);
    check(A != NULL, "invalid-config: library loaded");
    check(box64_signal_policy_set_library(A, 7) == -1,
          "invalid-config: out-of-range policy refused");
    check(box64_dlerror() != NULL, "invalid-config: error recorded");
    box64_lib_fini();
    return finish();
  }

  if (is_case(mode, "counter")) {
    void* zero;
    box64_signal_stats_t a, b;
    A = box64_dlopen(argv[1], RTLD_NOW);
    zero = A ? box64_dlsym(A, "g_zero") : NULL;
    check(zero != NULL, "counter: guest symbol resolved");
    if (zero) {
      box64_signal_stats_enable(1);
      memset(&a, 0, sizeof(a));
      box64_signal_stats(&a);
      (void)box64_call0(zero);
      memset(&b, 0, sizeof(b));
      box64_signal_stats(&b);
      check(b.setjmp_skips > a.setjmp_skips && b.setjmp_saves == a.setjmp_saves,
            "counter: global FAST call armed savemask=0");
      check(box64_signal_policy_set_library(A, BOX64_SIGNAL_POLICY_COMPAT) == 0,
            "counter: A set COMPAT");
      memset(&a, 0, sizeof(a));
      box64_signal_stats(&a);
      (void)box64_call0(zero);
      memset(&b, 0, sizeof(b));
      box64_signal_stats(&b);
      check(b.setjmp_saves > a.setjmp_saves && b.setjmp_skips == a.setjmp_skips,
            "counter: per-library COMPAT call armed savemask=1");
      check(box64_signal_policy_set_library(A, BOX64_SIGNAL_POLICY_FAST) == 0,
            "counter: A back to FAST (override cleared)");
      memset(&a, 0, sizeof(a));
      box64_signal_stats(&a);
      (void)box64_call0(zero);
      memset(&b, 0, sizeof(b));
      box64_signal_stats(&b);
      check(b.setjmp_skips > a.setjmp_skips && b.setjmp_saves == a.setjmp_saves,
            "counter: back on FAST the same entry skips the save");
    }
    box64_lib_fini();
    return finish();
  }

  if (is_case(mode, "invalid-handle")) {
    /* Load a guest first: endBox64()'s SerializeAllMapping() dereferences a
     * NULL mapping list when nothing was ever mapped (pre-existing path). */
    A = box64_dlopen(argv[1], RTLD_NOW);
    check(A != NULL, "invalid-handle: library loaded");
    check(box64_signal_policy_set_library((void*)0x1234,
                                          BOX64_SIGNAL_POLICY_COMPAT) == -1,
          "invalid-handle: bogus handle refused on set");
    check(box64_dlerror() != NULL, "invalid-handle: error recorded");
    check(box64_signal_policy_get_library(NULL) == -1,
          "invalid-handle: NULL handle refused on get");
    box64_lib_fini();
    return finish();
  }

  A = box64_dlopen(argv[1], RTLD_NOW);
  B = box64_dlopen(argv[2], RTLD_NOW);
  if (!A || !B) {
    const char* e = box64_dlerror(); /* read once: the slot is read-and-consume */
    printf("[signal-library] dlopen: %s\n", e ? e : "?");
    return 1;
  }
  /* With no environment rule both libraries get the global FAST default; the
   * env-driven cases assert their own expected values instead. */
  if (!getenv("BOX64DL_SIGNAL_LIBRARY_RULES"))
    check(box64_signal_policy_get_library(A) == BOX64_SIGNAL_POLICY_FAST &&
              box64_signal_policy_get_library(B) == BOX64_SIGNAL_POLICY_FAST,
          "both libraries default to FAST");

  if (is_case(mode, "precedence")) case_precedence(A, B);
  if (is_case(mode, "env-basename") || is_case(mode, "env-fullpath") ||
      is_case(mode, "env-nosubstring"))
    case_env(A, B, mode);
  if (is_case(mode, "threads")) {
    void* symA = box64_dlsym(A, "g_zero");
    void* symB = box64_dlsym(B, "g_sig_hits2");
    check(symA != NULL && symB != NULL, "threads: guest symbols resolved");
    if (symA && symB) case_threads(A, B, symA, symB);
  }
  if (is_case(mode, "unload-reload")) {
    void* sym = box64_dlsym(A, "g_zero");
    check(sym != NULL, "unload-reload: guest symbol resolved");
    check(box64_signal_policy_set_library(A, BOX64_SIGNAL_POLICY_COMPAT) == 0,
          "unload-reload: A set COMPAT");
    check_policy(box64_signal_policy_get_library(A), BOX64_SIGNAL_POLICY_COMPAT,
                 "unload-reload: override visible");
    box64_dlclose(A);
    A = box64_dlopen(argv[1], RTLD_NOW);
    check(A != NULL, "unload-reload: library reloaded");
    if (A)
      check_policy(box64_signal_policy_get_library(A), BOX64_SIGNAL_POLICY_FAST,
                   "unload-reload: reloaded library has the default policy");
  }
  if (is_case(mode, "mixed")) {
    void* install_b = box64_dlsym(B, "g_sig_install2");
    void* invoke_a = box64_dlsym(A, "g_invoke");
    void* fire_b = box64_dlsym(B, "g_sig_change_then_fire2");
    void* hits_b = box64_dlsym(B, "g_sig_hits2");
    check(install_b && invoke_a && fire_b && hits_b,
          "mixed: guest symbols resolved");
    if (install_b && invoke_a && fire_b && hits_b) {
      check(box64_call0(install_b) == 0,
            "mixed: guest SIGUSR1 handler installed (sa_mask=SIGALRM)");
      /* outer FAST, inner COMPAT: the inner frame must force a new jmpbuf and
       * the resume must restore the inner M0, not keep the outer FAST M1. */
      mixed_run(A, B, invoke_a, fire_b, hits_b, 1, 0, "mixed");
      /* and the reverse direction in the same process. */
      mixed_run(A, B, invoke_a, fire_b, hits_b, 0, 1, "mixed(rev)");
    }
  }

  box64_lib_fini();
  return finish();
}
