/*
 * Process-wide SIGNAL-ACTION regression: one logical action per signal, shared
 * by the native host and the guest (include/libbox64_signal_actions.h).
 *
 * WHAT THIS TESTS
 * ---------------
 * The API promises that a signal has ONE logical action for the whole process:
 *
 *   - it is initialised at preinit (inside box64_lib_init()) from the native
 *     action the APP had already installed, so the app's own handler is the
 *     starting value, not SIG_DFL;
 *   - a guest rt_sigaction(2) may replace it and a native
 *     box64_signal_action_set_native() may replace it, in either direction,
 *     with no isolation between the two views;
 *   - either side can query the other's action back, and what a guest reads
 *     for a NATIVE action is a STABLE, guest-callable bridge: two queries are
 *     byte-identical, calling it re-enters the ORIGINAL native function, and
 *     re-installing it round-trips back to a NATIVE action, not to a guest one;
 *   - the reported flags/mask are the APP's logical ones, not box64's own
 *     kernel arming, and they stay coherent for concurrent readers;
 *   - the engine never routes a guest fault through GS (the embedded GS=0 gap);
 *     the action decides the route.
 *
 * It also covers the four behaviours that make an action "logical" rather than
 * a thin sigaction() wrapper:
 *   - sa_mask / automatic self-block / SA_NODEFER nesting, checked from BOTH
 *     sides against the real rt_sigprocmask(2) the delivered handler observes;
 *   - SA_ONSTACK delivery onto the guest's and the native app's alt stack;
 *   - SA_RESTART vs EINTR on a real blocked read interrupted by a real signal;
 *   - SA_RESETHAND as a VIRTUAL one-shot: the kernel handler must NOT carry the
 *     kernel reset bit (the engine keeps its dispatcher installed), the logical
 *     action becomes DEFAULT after exactly one delivery, and a second delivery
 *     is fatal;
 *   - the GS=0 dynarec internal repair (a real RWX guest page patched in
 *     place) is handled INSIDE the engine: the host's SIGSEGV action never sees
 *     it and an app RESETHAND action is not consumed by it;
 *   - a guest hardware fault that a guest handler repairs with raw mprotect(2)
 *     resumes at the FAULTING instruction, with no native-PC fiction.
 *
 * PRINCIPLES
 * ----------
 *   - Everything is driven by signals the process REALLY receives. The guest
 *     fixture (tests/guest/signal_actions_guest.c) returns logic bitmaps, so
 *     the host verifies values across the engine boundary instead of trusting a
 *     guest printf.
 *   - Each case is a fresh process: the policy default and the action table are
 *     process-wide and one-shot. The Makefile runs BOX64_DYNAREC=0/1 crossed
 *     with BOX64DL_SIGNAL_POLICY=fast/compat; tests/signal-actions-cases.list
 *     is the exact matrix.
 *   - Any delivery that may be FATAL runs in a forked child that is reaped, so
 *     it can never take the harness down. The parent re-checks its own engine
 *     afterwards.
 *   - Non-fatal deliveries are driven so that a box64 jmpbuf unwind cannot
 *     abandon a frame the harness is standing on: either the guest handler
 *     leaves its ucontext untouched (a plain return), or the delivery is taken
 *     on a worker thread whose join is bounded by a deadline.
 *   - Nothing here is claimed to pass before the final engine build.
 *
 * Usage: test_signal_actions <guest.so> <case>
 *   cases: alive host-preinit guest-replace native-override unity roundtrip
 *          not-init invalid uc-mask longjmp flags-mask altstack restart resethand
 *          resethand-race smc pagefix native-fault native-c-fault chld
 *          concurrent fatal
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#include "libbox64.h"
#include "libbox64_signal_actions.h"
#include "call_compat.h"

/* On x86_64 SA_RESTORER is not part of the glibc <signal.h> interface; the
 * value is fixed by the ABI, and the logical action preserves it. */
#ifndef SA_RESTORER
#define SA_RESTORER 0x04000000
#endif

/* ------------------------------------------------------------------ *
 * Harness plumbing.
 * ------------------------------------------------------------------ */

static int g_fail;
static int g_checks;
static const char* g_case = "?";

static void check(int cond, const char* what) {
  g_checks++;
  if (!cond) {
    g_fail = 1;
    printf("[signal-actions] FAIL %s: %s\n", g_case, what);
  } else {
    printf("[signal-actions] ok   %s: %s\n", g_case, what);
  }
}

static void check_true(int cond, const char* what) { check(cond != 0, what); }

static void check_u64(uint64_t got, uint64_t want, const char* what) {
  if (got != want)
    printf("[signal-actions]      got=%#llx want=%#llx\n",
           (unsigned long long)got, (unsigned long long)want);
  check(got == want, what);
}

/* Bitmap comparison that names the missing bits. */
static void check_bits(uint64_t got, uint64_t want, const char* what) {
  int i;
  if (got != want) {
    printf("[signal-actions]      bitmap got=%#llx want=%#llx (missing:",
           (unsigned long long)got, (unsigned long long)want);
    for (i = 0; i < 64; ++i)
      if ((want & (1ull << i)) && !(got & (1ull << i))) printf(" b%d", i);
    printf("; extra:");
    for (i = 0; i < 64; ++i)
      if ((got & (1ull << i)) && !(want & (1ull << i))) printf(" b%d", i);
    printf(")\n");
  }
  check(got == want, what);
}

static int is_case(const char* mode) { return strcmp(g_case, mode) == 0; }

static void exit_result(void) {
  if (g_fail) {
    printf("[signal-actions] RESULT: FAIL (%s, %d checks)\n", g_case, g_checks);
    fflush(NULL);
    _exit(1);
  }
  printf("[signal-actions] RESULT: PASS (%s, %d checks)\n", g_case, g_checks);
  fflush(NULL);
  _exit(0);
}

static int action_mask_has(const box64_signal_action_t* a, int sig) {
  return sigismember(&a->mask, sig) == 1;
}

/* ------------------------------------------------------------------ *
 * Engine + guest handles.
 * ------------------------------------------------------------------ */

static void* g_lib;
static void* S(const char* n) { return box64_dlsym(g_lib, n); }

static void* sym_alive;
static void* sym_action_set;      /* g_action_set(sig,kind,h,r,flags,mask) */
static void* sym_query_size;      /* g_action_query_size */
static void* sym_roundtrip;       /* g_action_roundtrip */
static void* sym_query_stable;    /* g_action_query_stable */
static void* sym_action_save;     /* g_action_save */
static void* sym_action_restore;  /* g_action_restore_saved */
static void* sym_action_call_old; /* g_action_call_saved */
static void* sym_saved_handler;   /* g_action_saved_handler */
static void* sym_saved_flags;
static void* sym_saved_mask;
static void* sym_raise;
static void* sym_raise_and_wait;
static void* sym_hits_a;
static void* sym_hits_b;
static void* sym_handler_sig;
static void* sym_handler_frame;
static void* sym_mask_now;
static void* sym_nested_probe;
static void* sym_one_shot_probe;
static void* sym_altstack_probe;
static void* sym_altstack_base;
static void* sym_altstack_size;
static void* sym_altstack_get;
static void* sym_altstack_disable;
static void* sym_restart_install;
static void* sym_read_fd;
static void* sym_smc_probe;
static void* sym_fault_prepare;
static void* sym_fault_store;
static void* sym_uc_edit;          /* g_uc_edit_probe */
static void* sym_rereg;            /* g_one_shot_rereg_probe */
static void* sym_oneshot_which;    /* g_one_shot_which */
static void* sym_restorer_rt;      /* g_restorer_roundtrip */
static void* sym_rereg_one;        /* g_one_shot_rereg_one */
static void* sym_sigchld_probe;
static void* sym_nf_prepare;       /* g_natfault_prepare */
static void* sym_nf_chain_prepare; /* g_natfault_chain_prepare */
static void* sym_nf_page;          /* g_natfault_page */
static void* sym_nf_invoke;        /* g_natfault_invoke */
static void* sym_nf_fixed;         /* g_natfault_fixed */
static void* sym_nf_hits;          /* g_natfault_hits */
static void* sym_nf_chain_hits;    /* g_natfault_chain_hits */
static void* sym_fault_hopeless;   /* g_fault_hopeless_count */

/* Guest installation kinds (tests/guest/signal_actions_guest.c). */
#define GK_DFL 0
#define GK_IGN 1
#define GK_GUEST_A 2
#define GK_GUEST_B 3
#define GK_RAW 4
#define GK_PAGEFIX 5
#define GK_CHLD 6

/* g_action_set(sig, kind, handler, restorer, flags, mask) has six guest
 * arguments, so it needs the six-register entry. These two helpers keep the
 * call sites readable; the raw guest return value is passed through (negative
 * on a refusal). */
static uint64_t guest_action_set(int sig, int kind, uint64_t flags,
                                 uint64_t mask) {
  return test_simple6(sym_action_set, (uint64_t)(unsigned)sig,
                      (uint64_t)(unsigned)kind, 0, 0, flags, mask);
}

static uint64_t guest_action_set_full(int sig, int kind, uint64_t handler,
                                      uint64_t restorer, uint64_t flags,
                                      uint64_t mask) {
  return test_simple6(sym_action_set, (uint64_t)(unsigned)sig,
                      (uint64_t)(unsigned)kind, handler, restorer, flags, mask);
}

static int load_guest(const char* path) {
  g_lib = box64_dlopen(path, RTLD_NOW);
  if (!g_lib) {
    const char* e = box64_dlerror();
    printf("[signal-actions] dlopen(%s): %s\n", path, e ? e : "?");
    return -1;
  }
  sym_alive = S("g_probe_alive");
  sym_action_set = S("g_action_set");
  sym_query_size = S("g_action_query_size");
  sym_roundtrip = S("g_action_roundtrip");
  sym_query_stable = S("g_action_query_stable");
  sym_action_save = S("g_action_save");
  sym_action_restore = S("g_action_restore_saved");
  sym_action_call_old = S("g_action_call_saved");
  sym_saved_handler = S("g_action_saved_handler");
  sym_saved_flags = S("g_saved_flags");
  sym_saved_mask = S("g_saved_mask");
  sym_raise = S("g_raise");
  sym_raise_and_wait = S("g_raise_and_wait");
  sym_hits_a = S("g_action_hits_a");
  sym_hits_b = S("g_action_hits_b");
  sym_handler_sig = S("g_action_handler_sig");
  sym_handler_frame = S("g_action_handler_frame");
  sym_mask_now = S("g_mask_now");
  sym_nested_probe = S("g_nested_mask_probe");
  sym_one_shot_probe = S("g_one_shot_probe");
  sym_altstack_probe = S("g_altstack_delivery_probe");
  sym_altstack_base = S("g_altstack_base");
  sym_altstack_size = S("g_altstack_size");
  sym_altstack_get = S("g_altstack_get");
  sym_altstack_disable = S("g_altstack_disable");
  sym_restart_install = S("g_restart_install");
  sym_read_fd = S("g_read_fd");
  sym_smc_probe = S("g_smc_probe");
  sym_fault_prepare = S("g_fault_prepare");
  sym_fault_store = S("g_fault_store");
  sym_uc_edit = S("g_uc_edit_probe");
  sym_rereg = S("g_one_shot_rereg_probe");
  sym_oneshot_which = S("g_one_shot_which");
  sym_restorer_rt = S("g_restorer_roundtrip");
  sym_rereg_one = S("g_one_shot_rereg_one");
  sym_sigchld_probe = S("g_sigchld_probe");
  sym_nf_prepare = S("g_natfault_prepare");
  sym_nf_chain_prepare = S("g_natfault_chain_prepare");
  sym_nf_page = S("g_natfault_page");
  sym_nf_invoke = S("g_natfault_invoke");
  sym_nf_fixed = S("g_natfault_fixed");
  sym_nf_hits = S("g_natfault_hits");
  sym_nf_chain_hits = S("g_natfault_chain_hits");
  sym_fault_hopeless = S("g_fault_hopeless_count");

  if (!sym_alive || !sym_action_set || !sym_raise || !sym_hits_a ||
      !sym_mask_now) {
    const char* e = box64_dlerror();
    printf("[signal-actions] missing guest symbols: %s\n", e ? e : "?");
    return -1;
  }
  return 0;
}

/* ------------------------------------------------------------------ *
 * The APP's own native handlers, installed BEFORE box64_lib_init.
 * ------------------------------------------------------------------ */

static volatile sig_atomic_t g_app_hits;
static volatile sig_atomic_t g_app_sig;
static volatile sig_atomic_t g_app_frame;      /* handler frame is on our altstack */
static volatile sig_atomic_t g_app_saw_siginfo;
/* The restart reader's handler and the driving thread share this counter. */
static _Atomic int g_app_rt_hits;
/* Whether a NATIVE action received a real native ucontext_t (see app_segv_host). */
static volatile sig_atomic_t g_nf_native_uc_seen;
static volatile sig_atomic_t g_nf_native_pc_resolves;
static volatile uint64_t g_nf_native_pc;
static volatile sig_atomic_t g_host_segv_hits; /* host SIGSEGV action: must stay 0 */

static void* g_altstack;
static size_t g_altstack_len;

static int addr_in_native_altstack(const void* p) {
  const char* b = (const char*)g_altstack;
  const char* q = (const char*)p;
  return g_altstack && q >= b && q < b + g_altstack_len;
}

/* The APP handler the engine must adopt at preinit: SA_SIGINFO | SA_ONSTACK, an
 * explicit sa_mask bit and no SA_RESTART — the shape a real embedder installs. */
static void app_handler(int sig, siginfo_t* info, void* ucv) {
  int local = 0;
  g_app_hits++;
  g_app_sig = sig;
  g_app_frame = addr_in_native_altstack(&local);
  (void)info;
  (void)ucv;
}

/* A distinct native function, so a query can tell WHICH one is the action. */
static void app_variant_b(int sig, siginfo_t* info, void* ucv) {
  g_app_hits++;
  g_app_sig = (sig_atomic_t)(0x2000 + sig);
  (void)info;
  (void)ucv;
}

/* The host SIGSEGV action: a guest/JIT fault reaching it is a wrong route, so
 * it records and (in the SMC case) must stay at zero. */
static void app_segv_host(int sig, siginfo_t* info, void* ucv) {
  g_host_segv_hits++;
  g_app_sig = sig;
  g_app_saw_siginfo = info ? 1 : 0;
  /* Record whether the delivered context is a REAL NATIVE one. The engine hands
   * a NATIVE action the original host ucontext_t, not the x86-64 guest one, so
   * the faulting PC must be an address of THIS process's own code and must
   * resolve through dladdr(). A guest ucontext_t would carry a guest address
   * here instead. */
  if (ucv) {
    void* pc = NULL;
#if defined(__loongarch64)
    pc = (void*)((ucontext_t*)ucv)->uc_mcontext.__pc;
#elif defined(__x86_64__)
    pc = (void*)((ucontext_t*)ucv)->uc_mcontext.gregs[REG_RIP];
#endif
    if (pc) {
      Dl_info di;
      g_nf_native_pc = (uint64_t)(uintptr_t)pc;
      g_nf_native_pc_resolves = (dladdr(pc, &di) && di.dli_fname) ? 1 : 0;
      g_nf_native_uc_seen = 1;
    }
  }
}

static void app_restart_handler(int sig) {
  g_app_rt_hits++;
  (void)sig;
}

static void install_native_sigusr1(void (*fn)(int, siginfo_t*, void*), int flags,
                                   int mask_sig, int use_altstack) {
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_sigaction = fn;
  sa.sa_flags = SA_SIGINFO | flags;
  sigemptyset(&sa.sa_mask);
  if (mask_sig) sigaddset(&sa.sa_mask, mask_sig);
  if (use_altstack && g_altstack) {
    stack_t ss;
    ss.ss_sp = g_altstack;
    ss.ss_size = g_altstack_len;
    ss.ss_flags = 0;
    sigaltstack(&ss, NULL);
  }
  sigaction(SIGUSR1, &sa, NULL);
}

static void install_host_segv_watchdog(void) {
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_sigaction = app_segv_host;
  sa.sa_flags = SA_SIGINFO;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGSEGV, &sa, NULL);
}

/* Re-adopt the APP's native SIGSEGV action through the SUPPORTED native surface.
 *
 * The native-c-fault rounds run sequentially in one process and the PLAIN round
 * leaves a GUEST handler as the current action. The chaining round's guest code
 * saves "the action that is current before I replace it" and chains to it,
 * expecting the app's NATIVE action; without this call it would save the plain
 * round's own guest handler (a guest address) and the chain would re-enter this
 * fixture instead of the app's native handler, so the chaining assertions would
 * measure nothing. Re-installing through box64_signal_action_set_native() is the
 * documented way to make a native action current after init. */
static void readopt_app_segv_native(void) {
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_sigaction = app_segv_host;
  sa.sa_flags = SA_SIGINFO;
  sigemptyset(&sa.sa_mask);
  (void)box64_signal_action_set_native(SIGSEGV, &sa, NULL);
}

/* The app's SIGSEGV action used by the SMC case: SA_SIGINFO | SA_RESETHAND, so
 * the case can prove the engine's internal repair did not consume it. */
static void install_app_segv_resethand(void) {
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_sigaction = app_segv_host;
  sa.sa_flags = SA_SIGINFO | SA_RESETHAND;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGSEGV, &sa, NULL);
}

/* ------------------------------------------------------------------ *
 * Host mask helpers.
 * ------------------------------------------------------------------ */

static int host_mask_blocked(int sig) {
  sigset_t s;
  if (pthread_sigmask(SIG_SETMASK, NULL, &s) != 0) return -1;
  return sigismember(&s, sig) == 1;
}

static void host_mask_block(int sig) {
  sigset_t s;
  sigemptyset(&s);
  sigaddset(&s, sig);
  pthread_sigmask(SIG_BLOCK, &s, NULL);
}

static void host_mask_unblock(int sig) {
  sigset_t s;
  sigemptyset(&s);
  sigaddset(&s, sig);
  pthread_sigmask(SIG_UNBLOCK, &s, NULL);
}

static void host_mask_clear(void) {
  sigset_t s;
  sigemptyset(&s);
  pthread_sigmask(SIG_SETMASK, &s, NULL);
}

/* The 8-byte kernel sigset encoding the guest fixture uses (bit i = sig i+1). */
static uint64_t host_mask_word(void) {
  sigset_t s;
  uint64_t w = 0;
  int i;
  if (pthread_sigmask(SIG_SETMASK, NULL, &s) != 0) return 0;
  for (i = 1; i < 32; ++i)
    if (sigismember(&s, i) == 1) w |= (1ull << (i - 1));
  return w;
}

/* ------------------------------------------------------------------ *
 * Deadline-bounded worker driver.
 * ------------------------------------------------------------------ */

struct worker {
  pthread_t th;
  void* (*fn)(void*);
  void* arg;
  int done;
  int timed_out;
  int rc;
};

static void* worker_trampoline(void* p) {
  struct worker* w = (struct worker*)p;
  w->rc = (int)(intptr_t)w->fn(w->arg);
  w->done = 1;
  return NULL;
}

/* Run fn on its own thread and wait at most ms. Returns 0 joined, 1 timed out
 * (the thread is left running: the caller is about to report and exit), -1 on
 * create/join failure. */
static int run_bounded(struct worker* w, void* (*fn)(void*), void* arg, int ms) {
  struct timespec ts;
  memset(w, 0, sizeof(*w));
  w->fn = fn;
  w->arg = arg;
  if (pthread_create(&w->th, NULL, worker_trampoline, w) != 0) return -1;
  clock_gettime(CLOCK_REALTIME, &ts);
  ts.tv_sec += ms / 1000;
  ts.tv_nsec += (long)(ms % 1000) * 1000000L;
  if (ts.tv_nsec >= 1000000000L) {
    ts.tv_sec++;
    ts.tv_nsec -= 1000000000L;
  }
  {
    int rc = pthread_timedjoin_np(w->th, NULL, &ts);
    if (rc == 0) return 0;
    if (rc == ETIMEDOUT) {
      w->timed_out = 1;
      return 1;
    }
    return -1;
  }
}

/* ------------------------------------------------------------------ *
 * Forked child plumbing: anything that may terminate runs in a child.
 * ------------------------------------------------------------------ */

typedef int (*child_fn)(void*);

/* Fork, run fn(arg) in the child and _exit with its low byte, then reap it. */
static int run_child(child_fn fn, void* arg, int* out_status) {
  pid_t pid = fork();
  int status = 0;
  if (pid < 0) return -1;
  if (pid == 0) {
    int rc = fn(arg);
    fflush(NULL);
    _exit(rc & 0xff);
  }
  if (waitpid(pid, &status, 0) != pid) return -1;
  if (out_status) *out_status = status;
  return 0;
}

static int child_raise_sigusr1(void* arg) {
  (void)arg;
  test_simple1(sym_raise_and_wait, SIGUSR1);
  return 0;
}

static int child_default_segv(void* arg) {
  (void)arg;
  *(volatile int*)0 = 1; /* real hardware fault, SIGSEGV at DEFAULT */
  return 0;
}

/* ------------------------------------------------------------------ *
 * Engine bring-up.
 * ------------------------------------------------------------------ */

static int engine_up(int want_app_action, const char* guest) {
  if (want_app_action)
    install_native_sigusr1(app_handler, SA_ONSTACK, SIGUSR2, 1);

  if (box64_lib_init() != 0 || box64_ensure_emu() != 0) {
    const char* e = box64_dlerror();
    printf("[signal-actions] engine init: %s\n", e ? e : "?");
    return -1;
  }
  if (load_guest(guest) != 0) return -1;
  check_u64(test_simple0(sym_alive), 0x5a17,
            "guest fixture runs through the engine");
  return 0;
}

/* ================================================================== *
 * Case: host-preinit — the app's pre-init native action is the initial one.
 * ================================================================== */

static void verify_app_action_is_current(const char* tag) {
  box64_signal_action_t act;
  char buf[160];

  memset(&act, 0, sizeof(act));
  snprintf(buf, sizeof(buf), "%s: query of SIGUSR1 succeeds", tag);
  check(box64_signal_action_get(SIGUSR1, &act) == 0, buf);
  snprintf(buf, sizeof(buf), "%s: the action is NATIVE", tag);
  check(act.kind == BOX64_SIGNAL_ACTION_NATIVE, buf);
  snprintf(buf, sizeof(buf), "%s: it names the app's own function", tag);
  check_u64(act.handler, (uint64_t)(uintptr_t)&app_handler, buf);
  snprintf(buf, sizeof(buf), "%s: SA_SIGINFO survived", tag);
  check((act.flags & SA_SIGINFO) != 0, buf);
  snprintf(buf, sizeof(buf), "%s: the app's explicit sa_mask bit survived", tag);
  check(action_mask_has(&act, SIGUSR2), buf);
}

static void case_host_preinit(void) {
  verify_app_action_is_current("host-preinit");

  /* The GUEST must see the same action as a stable, guest-callable bridge. */
  check_u64(test_simple1(sym_query_stable, SIGUSR1), 1,
            "guest's two old-action queries of a native action are identical");
  check_u64(test_simple1(sym_query_stable, SIGUSR1), 1,
            "stable bridge confirmed on a second pair of queries");

  /* The guest calls the old action (chaining): for a native action this must
   * land in the ORIGINAL native function, not in a guest stub. */
  g_app_hits = 0;
  g_app_sig = 0;
  check_true(test_simple1(sym_action_save, SIGUSR1) != 0,
             "guest saved the current native action");
  check_u64(test_simple1(sym_action_call_old, SIGUSR1), 1,
            "guest called the saved native action through the bridge");
  check(g_app_hits >= 1, "the NATIVE app handler really ran for the guest");
  check(g_app_sig == SIGUSR1 || g_app_sig == 0x2000 + SIGUSR1,
        "the native handler saw the chained signal");

  /* Re-installing exactly what the guest read must round-trip to NATIVE. */
  {
    box64_signal_action_t after;
    check_u64(test_simple1(sym_action_restore, SIGUSR1), 0,
              "guest re-installed the saved native action verbatim");
    memset(&after, 0, sizeof(after));
    check(box64_signal_action_get(SIGUSR1, &after) == 0,
          "query after the guest restore succeeds");
    check(after.kind == BOX64_SIGNAL_ACTION_NATIVE,
          "the guest round trip did not isolate: the action is still NATIVE");
    check_u64(after.handler, (uint64_t)(uintptr_t)&app_handler,
              "the round tripped action still names the app function");
  }

  /* A real raise must reach the app handler again, through the bridge. */
  {
    uint64_t before = (uint64_t)g_app_hits;
    check_u64(test_simple1(sym_raise, SIGUSR1), 0,
              "real tgkill(SIGUSR1) accepted");
    check((uint64_t)g_app_hits > before,
          "a real delivery after the guest round trip reached the app handler");
  }
}

/* ================================================================== *
 * Case: guest-replace — a guest action replaces a native one, in both
 * directions, and the real delivery follows the CURRENT action.
 * ================================================================== */

static void case_guest_replace(void) {
  box64_signal_action_t act;

  check_u64(guest_action_set(SIGUSR1, GK_GUEST_A, 0, 0), 0,
            "guest replaced the native SIGUSR1 action");

  memset(&act, 0, sizeof(act));
  check(box64_signal_action_get(SIGUSR1, &act) == 0, "query after the replace");
  check(act.kind == BOX64_SIGNAL_ACTION_GUEST,
        "the logical action is GUEST after a guest sigaction");
  check(act.handler != 0, "the guest action carries the guest handler value");

  /* A REAL native raise now invokes the GUEST handler, not the old native one. */
  {
    uint64_t app_before = (uint64_t)g_app_hits;
    uint64_t hits_before = test_simple0(sym_hits_a);
    check_u64(test_simple1(sym_raise, SIGUSR1), 0,
              "real tgkill under the guest action accepted");
    check(test_simple0(sym_hits_a) > hits_before,
          "the real delivery invoked the GUEST handler");
    check((uint64_t)g_app_hits == app_before,
          "the replaced native handler was NOT invoked");
  }

  /* The native view must not show the raw guest function pointer as if it were
   * a host function. */
  check(test_simple1(sym_action_save, SIGUSR1) != 0,
        "guest can still query the guest action");

  /* Guest IGNORE / DEFAULT are first-class kinds, not "guest". */
  check_u64(guest_action_set(SIGUSR1, GK_IGN, 0, 0), 0,
            "guest installed SIG_IGN");
  memset(&act, 0, sizeof(act));
  check(box64_signal_action_get(SIGUSR1, &act) == 0, "query after guest IGNORE");
  check(act.kind == BOX64_SIGNAL_ACTION_IGNORE, "guest IGNORE reads as IGNORE");

  check_u64(guest_action_set(SIGUSR1, GK_DFL, 0, 0), 0,
            "guest installed SIG_DFL");
  memset(&act, 0, sizeof(act));
  check(box64_signal_action_get(SIGUSR1, &act) == 0, "query after guest DEFAULT");
  check(act.kind == BOX64_SIGNAL_ACTION_DEFAULT, "guest DFL reads as DEFAULT");
}

/* ================================================================== *
 * Case: native-override — one process-wide action, no isolation.
 * ================================================================== */

/* Install app_variant_b as the SIGUSR1 native action and report whether the
 * setter accepted it. Shared by the cases that need a SECOND distinct native
 * handler (so a query can tell which one is current). */
static int install_native_variant_b(void) {
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_sigaction = app_variant_b;
  sa.sa_flags = SA_SIGINFO;
  sigemptyset(&sa.sa_mask);
  sigaddset(&sa.sa_mask, SIGALRM);
  return box64_signal_action_set_native(SIGUSR1, &sa, NULL);
}

static void case_native_override(void) {
  box64_signal_action_t act, old;
  struct sigaction sa;

  /* guest action first */
  check_u64(guest_action_set(SIGUSR1, GK_GUEST_A, 0, 0), 0,
            "guest installed its action first");
  memset(&act, 0, sizeof(act));
  check(box64_signal_action_get(SIGUSR1, &act) == 0, "query sees the guest action");
  check(act.kind == BOX64_SIGNAL_ACTION_GUEST, "kind is GUEST");

  /* native overrides it (via the shared helper, which installs app_variant_b
   * with an explicit sa_mask so the guest can see BOTH the handler and the
   * mask round-trip) */
  (void)sa;
  memset(&old, 0, sizeof(old));
  {
    struct sigaction nat;
    memset(&nat, 0, sizeof(nat));
    nat.sa_sigaction = app_variant_b;
    nat.sa_flags = SA_SIGINFO;
    sigemptyset(&nat.sa_mask);
    sigaddset(&nat.sa_mask, SIGALRM);
    check(box64_signal_action_set_native(SIGUSR1, &nat, &old) == 0,
          "native set_native() overrode the guest action");
  }
  check(old.kind == BOX64_SIGNAL_ACTION_GUEST,
        "set_native() reported the previous action as GUEST");

  memset(&act, 0, sizeof(act));
  check(box64_signal_action_get(SIGUSR1, &act) == 0, "query after native override");
  check(act.kind == BOX64_SIGNAL_ACTION_NATIVE, "kind is NATIVE after the override");
  check_u64(act.handler, (uint64_t)(uintptr_t)&app_variant_b,
            "the handler is the new native function (no isolation)");
  check(action_mask_has(&act, SIGALRM),
        "the new native action's sa_mask bit is reported");

  /* the guest sees the same new action, with its logical flags and mask */
  check_u64(test_simple1(sym_query_stable, SIGUSR1), 1,
            "the guest query stays stable after the native override");
  check_true(test_simple1(sym_action_save, SIGUSR1) != 0,
             "the guest saved the native action");
  check(((uint64_t)test_simple0(sym_saved_flags) & SA_SIGINFO) != 0,
        "the guest sees SA_SIGINFO from the native action");
  check(((uint64_t)test_simple0(sym_saved_mask) & (1ull << (SIGALRM - 1))) != 0,
        "the guest sees the native action's explicit sa_mask bit");

  /* and back */
  check_u64(guest_action_set(SIGUSR1, GK_GUEST_A, 0, 0), 0,
            "guest overrode the native action");
  memset(&act, 0, sizeof(act));
  check(box64_signal_action_get(SIGUSR1, &act) == 0, "query after the second flip");
  check(act.kind == BOX64_SIGNAL_ACTION_GUEST,
        "the single process-wide action flipped back to GUEST");

  /* a real delivery follows the CURRENT action, whichever side set it */
  {
    uint64_t before = (uint64_t)g_app_hits;
    uint64_t hb = test_simple0(sym_hits_a);
    check_u64(test_simple1(sym_raise, SIGUSR1), 0,
              "real raise after the flip accepted");
    check(test_simple0(sym_hits_a) > hb, "the guest action received the delivery");
    check((uint64_t)g_app_hits == before,
          "the overridden native action did NOT receive the delivery");
  }

  /* The generation advances on every replacement, so a concurrent reader can
   * tell two coherent snapshots apart. */
  {
    uint64_t g1, g2;
    memset(&act, 0, sizeof(act));
    (void)box64_signal_action_get(SIGUSR1, &act);
    g1 = act.generation;
    check_u64(guest_action_set(SIGUSR1, GK_GUEST_A, 0, 0), 0,
              "re-replace the action");
    memset(&act, 0, sizeof(act));
    (void)box64_signal_action_get(SIGUSR1, &act);
    g2 = act.generation;
    check(g2 > g1, "the generation advanced on the replacement");
  }
}

/* ================================================================== *
 * Case: unity — both directions in one process, ending back on the native app
 * action, and the real delivery following it.
 * ================================================================== */

static void case_unity(void) {
  /* 1. guest replaces the app's native action */
  check_u64(guest_action_set(SIGUSR1, GK_GUEST_A, 0, 0), 0,
            "unity: guest replaced the app's native action");
  {
    uint64_t app_before = (uint64_t)g_app_hits;
    uint64_t hb = test_simple0(sym_hits_a);
    check_u64(test_simple1(sym_raise, SIGUSR1), 0, "unity: real raise");
    check(test_simple0(sym_hits_a) > hb, "unity: the guest action got it");
    check((uint64_t)g_app_hits == app_before,
          "unity: the native action did not");
  }

  /* 2. native re-installs the app action, which must be the logical action
   *    again and must receive the next real delivery */
  {
    struct sigaction sa;
    box64_signal_action_t old;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = app_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    sigaddset(&sa.sa_mask, SIGUSR2);
    memset(&old, 0, sizeof(old));
    check(box64_signal_action_set_native(SIGUSR1, &sa, &old) == 0,
          "unity: native setter re-installed the app's ONSTACK action");
    check(old.kind == BOX64_SIGNAL_ACTION_GUEST,
          "unity: the setter reported the replaced action as GUEST");
  }
  verify_app_action_is_current("unity");
  {
    uint64_t before = (uint64_t)g_app_hits;
    uint64_t hb = test_simple0(sym_hits_a);
    check_u64(test_simple1(sym_raise, SIGUSR1), 0, "unity: real raise again");
    check((uint64_t)g_app_hits > before, "unity: the native action got it");
    check(test_simple0(sym_hits_a) == hb, "unity: the guest action did not");
  }

  /* 3. the guest can still read the native action back */
  check_u64(test_simple1(sym_query_stable, SIGUSR1), 1,
            "unity: the guest sees a stable native action");

  /* 4. a SECOND distinct native handler replaces it, and the guest sees THAT
   *    one: the native side is not pinned to the preinit function. */
  check(install_native_variant_b() == 0,
        "unity: a second native handler became the action");
  {
    box64_signal_action_t a;
    memset(&a, 0, sizeof(a));
    check(box64_signal_action_get(SIGUSR1, &a) == 0, "unity: query the second one");
    check_u64(a.handler, (uint64_t)(uintptr_t)&app_variant_b,
              "unity: the action names the second native handler");
  }
  {
    uint64_t before = (uint64_t)g_app_hits;
    g_app_sig = 0;
    check_u64(test_simple1(sym_raise, SIGUSR1), 0, "unity: real raise, third time");
    check((uint64_t)g_app_hits > before, "unity: the second native handler ran");
    check(g_app_sig == 0x2000 + SIGUSR1,
          "unity: it was app_variant_b, not the preinit handler");
  }
}

/* ================================================================== *
 * Case: roundtrip — the four fields survive a guest query/reinstall/query.
 * ================================================================== */

static void case_roundtrip(void) {
  verify_app_action_is_current("roundtrip");
  check_bits(test_simple1(sym_roundtrip, SIGUSR1), 0xf,
            "guest query/reinstall/query round-trips handler, flags, restorer, mask");

  /* SA_RESTORER is part of the LOGICAL action: the app registered a restorer
   * address, box64 strips both the flag and the address from the KERNEL action,
   * and the logical API must preserve them anyway. The address used here is a
   * CALLABLE guest entry (the fixture's own g_probe_alive); the case only
   * round-trips the VALUE and never executes it. */
  {
    void* pagefix = S("g_pagefix_handler_addr");
    uint64_t addr = pagefix ? (uint64_t)(uintptr_t)pagefix : (uint64_t)1;
    check_bits(test_simple2(sym_restorer_rt, SIGWINCH, addr), 0x7f,
               "SA_RESTORER and the restorer ADDRESS survive a query/reinstall/query");
    /* ... while the KERNEL action really has neither, which is WHY the logical
     * layer has to remember them. */
    {
      struct sigaction cur;
      memset(&cur, 0, sizeof(cur));
      check(sigaction(SIGWINCH, NULL, &cur) == 0,
            "host can query the kernel SIGWINCH action");
      check((cur.sa_flags & SA_RESTORER) == 0,
            "the KERNEL action carries no SA_RESTORER (the logical layer does)");
    }
  }

  /* A guest-provided RESTORER address (G_KIND_RAW) must reach the query too:
   * the logical action models what the app registered, not what the kernel
   * ended up with. Nothing here executes the restorer. */
  {
    uint64_t addr = (uint64_t)(uintptr_t)S("g_probe_alive");
    uint64_t flags = (uint64_t)(SA_SIGINFO | SA_RESTORER);
    check_u64(guest_action_set_full(SIGWINCH, GK_RAW, addr, addr, flags, 0), 0,
              "guest installed a RAW action with an explicit restorer address");
    check_u64(test_simple2(sym_restorer_rt, SIGWINCH, addr) & 0x1ull, 0x1ull,
              "the RAW installation is visible to the guest query");
  }
}

/* ================================================================== *
 * Case: invalid — refusals, including the sigsetsize and sig 0/KILL/STOP.
 * ================================================================== */

static void case_invalid(void) {
  box64_signal_action_t act;
  struct sigaction sa;
  char buf[32];

  /* QUERY-ONLY calls are VALID for every signal: on Linux sigaction(sig, NULL,
   * &old) only reads the disposition, and SIGKILL/SIGSTOP have a well-defined
   * one. The header states the same: "Linux rejects SIGKILL/SIGSTOP for
   * rt_sigaction outright, so the setter does too; the getter still answers for
   * them (they are valid signals whose action can only ever be DEFAULT)." */
  memset(&act, 0, sizeof(act));
  check(box64_signal_action_get(SIGKILL, &act) == 0,
        "get(SIGKILL) succeeds: a query is legal for an unchangeable signal");
  check(act.kind == BOX64_SIGNAL_ACTION_DEFAULT, "SIGKILL's action is DEFAULT");
  memset(&act, 0, sizeof(act));
  check(box64_signal_action_get(SIGSTOP, &act) == 0,
        "get(SIGSTOP) succeeds: a query is legal for an unchangeable signal");
  check(act.kind == BOX64_SIGNAL_ACTION_DEFAULT, "SIGSTOP's action is DEFAULT");

  /* ERRNO CONTRACT: EFAULT (NULL out), EINVAL (out of range), ENOTSUP (32/33).
   * Asserting only "non-zero" would let any of the three masquerade as another. */
  errno = 0;
  check(box64_signal_action_get(SIGUSR1, NULL) == -1 && errno == EFAULT,
        "get(NULL out) fails with EFAULT");
  errno = 0;
  check(box64_signal_action_get(0, &act) == -1 && errno == EINVAL,
        "get(sig 0) fails with EINVAL");
  errno = 0;
  check(box64_signal_action_get(-1, &act) == -1 && errno == EINVAL,
        "get(sig -1) fails with EINVAL");
  errno = 0;
  check(box64_signal_action_get(BOX64DL_SIGNAL_MAX + 1, &act) == -1 &&
            errno == EINVAL,
        "get(sig BOX64DL_SIGNAL_MAX+1) fails with EINVAL");
  /* BOX64DL_SIGNAL_MAX is 64, so 65 is past the top of the tracked range. */
  check(BOX64DL_SIGNAL_MAX == 64, "the tracked range tops out at 64");

  /* 32/33 are the C library's internal pthread signals: every entry point
   * rejects them with ENOTSUP rather than overwriting glibc's own handlers. */
  errno = 0;
  check(box64_signal_action_get(32, &act) == -1 && errno == ENOTSUP,
        "get(sig 32) is refused with ENOTSUP (glibc's internal signal)");
  errno = 0;
  check(box64_signal_action_get(33, &act) == -1 && errno == ENOTSUP,
        "get(sig 33) is refused with ENOTSUP (glibc's internal signal)");

  /* A non-NULL act that tries to CHANGE SIGKILL/SIGSTOP is EINVAL, and so is an
   * out-of-range or reserved signal. */
  memset(&sa, 0, sizeof(sa));
  sa.sa_sigaction = app_variant_b;
  sa.sa_flags = SA_SIGINFO;
  errno = 0;
  check(box64_signal_action_set_native(SIGKILL, &sa, NULL) == -1 && errno == EINVAL,
        "set_native(SIGKILL) fails with EINVAL");
  errno = 0;
  check(box64_signal_action_set_native(SIGSTOP, &sa, NULL) == -1 && errno == EINVAL,
        "set_native(SIGSTOP) fails with EINVAL");
  errno = 0;
  check(box64_signal_action_set_native(0, &sa, NULL) == -1 && errno == EINVAL,
        "set_native(sig 0) fails with EINVAL");
  errno = 0;
  check(box64_signal_action_set_native(32, &sa, NULL) == -1 && errno == ENOTSUP,
        "set_native(sig 32) is refused with ENOTSUP");
  errno = 0;
  check(box64_signal_action_set_native(33, &sa, NULL) == -1 && errno == ENOTSUP,
        "set_native(sig 33) is refused with ENOTSUP");

  /* act == NULL is a QUERY, not a refusal: it changes nothing and fills *old
   * when non-NULL. act == NULL with old == NULL is a successful no-op. */
  {
    box64_signal_action_t oldq;
    memset(&oldq, 0, sizeof(oldq));
    errno = 0;
    check(box64_signal_action_set_native(SIGWINCH, NULL, &oldq) == 0,
          "set_native(sig, NULL, &old) is a successful query");
    check(oldq.kind == BOX64_SIGNAL_ACTION_DEFAULT,
          "the query filled *old with the current action");
    errno = 0;
    check(box64_signal_action_set_native(SIGWINCH, NULL, NULL) == 0,
          "set_native(sig, NULL, NULL) is a successful no-op");
    /* ... and the query really changed nothing. */
    memset(&oldq, 0, sizeof(oldq));
    check(box64_signal_action_get(SIGWINCH, &oldq) == 0 &&
              oldq.kind == BOX64_SIGNAL_ACTION_DEFAULT,
          "the NULL-act query left the action untouched");
  }

  /* The guest's own rt_sigaction follows the same rule: query valid, change
   * refused. */
  check((int64_t)guest_action_set(SIGKILL, GK_GUEST_A, 0, 0) < 0,
        "guest rt_sigaction(SIGKILL, act) is refused");
  check((int64_t)guest_action_set(SIGSTOP, GK_GUEST_A, 0, 0) < 0,
        "guest rt_sigaction(SIGSTOP, act) is refused");

  /* A sigsetsize the x86_64 ABI does not accept is refused in every case,
   * including the query. */
  {
    int64_t r = (int64_t)test_simple3(sym_query_size, SIGUSR1,
                                      (uint64_t)(uintptr_t)buf, 4);
    check(r < 0, "guest rt_sigaction sigsetsize=4 is refused");
    r = (int64_t)test_simple3(sym_query_size, SIGUSR1,
                              (uint64_t)(uintptr_t)buf, 16);
    check(r < 0, "guest rt_sigaction sigsetsize=16 is refused");
    r = (int64_t)test_simple3(sym_query_size, SIGUSR1,
                              (uint64_t)(uintptr_t)buf, 8);
    check(r == 0, "guest rt_sigaction sigsetsize=8 is accepted");
    r = (int64_t)test_simple3(sym_query_size, 0, (uint64_t)(uintptr_t)buf, 8);
    check(r < 0, "guest rt_sigaction(sig 0) query is refused");
  }

  /* A signal that was never armed still queries coherently. */
  memset(&act, 0, sizeof(act));
  check(box64_signal_action_get(SIGWINCH, &act) == 0,
        "query of an untouched signal succeeds");
  check(act.kind == BOX64_SIGNAL_ACTION_DEFAULT, "an untouched signal is DEFAULT");

  /* The flags field is the LOGICAL application value widened as
   * (uint64_t)(uint32_t). SA_RESETHAND is 0x80000000, whose sign bit must NOT
   * have been extended into the upper half. */
  {
    box64_signal_action_t r;
    check_u64(guest_action_set(SIGUSR2, GK_GUEST_A, (uint64_t)SA_RESETHAND, 0),
              0, "installed an action carrying SA_RESETHAND");
    memset(&r, 0, sizeof(r));
    check(box64_signal_action_get(SIGUSR2, &r) == 0, "query the SA_RESETHAND action");
    check((r.flags & 0x80000000ull) != 0,
          "SA_RESETHAND survived in the low 32 bits of flags");
    check((r.flags >> 32) == 0,
          "flags has no sign-extension garbage above bit 31 "
          "(the uint32 widening contract)");
  }
}

/* ================================================================== *
 * Case: not-init — every entry point reports ENOSYS before init and after
 * fini. This case must run WITHOUT box64_lib_init(); main dispatches it before
 * the engine comes up.
 * ================================================================== */

static void case_not_init(void) {
  box64_signal_action_t act;
  struct sigaction sa;

  memset(&act, 0, sizeof(act));
  errno = 0;
  check(box64_signal_action_get(SIGUSR1, &act) == -1 && errno == ENOSYS,
        "before init: get() fails with ENOSYS");
  memset(&sa, 0, sizeof(sa));
  sa.sa_sigaction = app_variant_b;
  sa.sa_flags = SA_SIGINFO;
  errno = 0;
  check(box64_signal_action_set_native(SIGUSR1, &sa, NULL) == -1 &&
            errno == ENOSYS,
        "before init: set_native() fails with ENOSYS");

  /* Pre-init, the argument validation still runs first for an out-of-range
   * signal: an uninitialised module must not report EINVAL for a valid signal
   * nor ENOSYS for an invalid one. */
  errno = 0;
  check(box64_signal_action_get(0, &act) == -1 && errno == EINVAL,
        "before init: an out-of-range signal is still EINVAL, not ENOSYS");
}

/* ================================================================== *
 * Case: uc-mask — an edit of the DELIVERED guest ucontext's mask (a changed
 * guest context, so box64 resumes with siglongjmp) must be preserved
 * IDENTICALLY under FAST and COMPAT.
 * ================================================================== */

/* One uc-edit delivery, with the host's own mask checked on both sides of it.
 * `want` is the expected bitmap; the SAME `want` is used for both policies,
 * because preserving the handler's uc_sigmask edit is the contract for both. */
/* One uc-edit delivery, with the host's own mask checked on BOTH sides of it.
 *
 * `want_bm` is the FULL 10-bit bitmap g_uc_edit_probe must return.
 * The host independently re-derives the same facts from its OWN pthread_sigmask,
 * so a guest that lied about its own mask could not pass. `pre` is the mask the
 * HOST blocks before the call; `extra` is what the handler blocks itself.
 *
 * `expect_edit_blocked` selects the required post-state of the uc_mask bits:
 * +1 = they must be BLOCKED, -1 = they must be CLEAR, 0 = not asserted. */
static void uc_round(const char* tag, uint64_t flags, uint64_t edit,
                     uint64_t uc_mask, uint64_t pre, uint64_t extra,
                     uint64_t want_bm, int expect_edit_blocked) {
  uint64_t bm;
  uint64_t host_w;
  uint64_t sig_bit = 1ull << (SIGUSR1 - 1);
  char buf[220];

  host_mask_clear();
  if (pre) {
    int ps;
    for (ps = 1; ps < 32; ++ps)
      if (pre & (1ull << (ps - 1))) host_mask_block(ps);
  }

  /* ucontext edits need the explicit 3-argument ABI; the fixture sets
   * SA_SIGINFO for any non-zero edit itself, so the flags here only carry what
   * the CALLER wants to add. */
  bm = test_simple6(sym_uc_edit, SIGUSR1, flags, edit, uc_mask, pre, extra);

  /* The fixture reports one 10-bit bitmap; compare it whole, so a bit in the
   * wrong half cannot pass by accident. */
  snprintf(buf, sizeof(buf), "%s: guest bitmap (see g_uc_edit_probe)", tag);
  check_bits(bm, want_bm, buf);

  /* The host's own view AFTER the delivery. */
  host_w = host_mask_word();
  if (expect_edit_blocked > 0) {
    snprintf(buf, sizeof(buf), "%s: host mask has the edited bits BLOCKED", tag);
    check(uc_mask && (host_w & uc_mask) == uc_mask, buf);
  } else if (expect_edit_blocked < 0) {
    snprintf(buf, sizeof(buf), "%s: host mask has the edited bits CLEAR", tag);
    check((host_w & uc_mask) == 0, buf);
  }

  snprintf(buf, sizeof(buf),
           "%s: the automatic self-block did not leak to the host", tag);
  check((host_w & sig_bit) == 0, buf);

  /* An ORDINARY handler-added bit must NOT survive: identical requirement to the
   * flags-mask case, and the requirement the header's mask-restore contract
   * implies for a normal return. */
  if (extra) {
    snprintf(buf, sizeof(buf),
             "%s: the handler's ordinary SIG_BLOCK additions were cleared", tag);
    check((host_w & extra) == 0, buf);
  }

  /* The application's own pre-call bits survive, EXCEPT the ones the handler's
   * ucontext edit explicitly cleared. */
  {
    uint64_t keep = pre;
    if ((int)edit == 2) keep &= ~uc_mask;
    if (keep) {
      snprintf(buf, sizeof(buf),
               "%s: the app's own pre-call bits survived the delivery", tag);
      check((host_w & keep) == keep, buf);
    }
  }
  host_mask_clear();
}

/* ================================================================== *
 * Case: uc-mask — a guest UCONTEXT edit survives; an ordinary handler
 * sigprocmask addition does NOT; the application's own pre-call mask is left
 * alone. Identical under FAST and COMPAT.
 * ================================================================== */

static void case_uc_mask(void) {
  uint64_t alarm_bit = 1ull << (SIGALRM - 1);
  uint64_t usr2_bit = 1ull << (SIGUSR2 - 1);
  uint64_t chld_bit = 1ull << (SIGCHLD - 1);

  /* Frozen requirement split:
   *   - only a guest UCONTEXT edit survives the delivery;
   *   - an ordinary handler rt_sigprocmask(2) addition is CLEARED, exactly like
   *     the kernel's own automatic self-block (the same rule flags-mask asserts
   *     from both sides);
   *   - the application's pre-existing mask is preserved, unless the handler's
   *     ucontext edit explicitly clears it.
   * Conflating "the handler blocked X" with "the app had X blocked" hid a bug:
   * the two have OPPOSITE required outcomes, so they are separate inputs.
   *
   * The case asserts the SEMANTIC outcome only and is IDENTICAL under
   * FAST and COMPAT; it does NOT require a mask-only edit to take the
   * siglongjmp resume -- `longjmp` measures that with a combined edit.
   *
   * Bitmap bits (g_uc_edit_probe): 0 ran, 1 saw pre, 2 saw self-block,
   * 3 saw extra, 4 uc-mask blocked after, 5 uc-mask clear after,
   * 6 extra cleared after, 7 pre kept after, 8 self-block gone, 9 same thread. */

  /* 1. No edit: plain return. The handler saw the app's own pre-call bit and
   *    its auto-block; afterwards the pre bit is kept and the auto-block is
   *    gone. */
  uc_round("no-edit", 0, 0, 0, usr2_bit, 0, 0x387, 0);

  /* 2. OR SIGALRM into the delivered uc_sigmask: THE EDIT SURVIVES. */
  uc_round("uc-mask-or", 0, 1, alarm_bit, 0, 0, 0x395, 1);

  /* 3. CLEAR SIGUSR2 from the delivered uc_sigmask while the app had it
   *    blocked: THE CLEARING SURVIVES, and that pre bit is expected to be gone
   *    because the handler's own ucontext edit removed it. */
  uc_round("uc-mask-clear", 0, 2, usr2_bit, usr2_bit, 0, 0x3A7, -1);

  /* 4. The handler's OWN ordinary rt_sigprocmask(SIG_BLOCK) must NOT survive.
   *    This is the requirement the header's ordinary-mask-restore contract
   *    implies, and the same one flags-mask asserts. */
  uc_round("handler-raw-block", 0, 0, 0, 0, chld_bit, 0x3CD, 0);

  /* 5. A register-only edit completes and returns normally; it changes no mask
   *    bit. */
  uc_round("r11-perturb", 0, 3, 0, 0, 0, 0x385, 0);

  /* 6. COMBINED edit (register + mask): the mask part must survive. */
  uc_round("combined", 0, 4, alarm_bit, 0, 0, 0x395, 1);

  /* 7. BOTH RULES AT ONCE — the case that catches the conflation: the app had
   *    SIGUSR2 blocked, the handler blocks SIGCHLD itself, and its ucontext edit
   *    ORs SIGALRM. Only the uc edit may survive; the pre bit is kept and the
   *    handler's own addition is cleared. */
  uc_round("pre+extra+uc", 0, 1, alarm_bit, usr2_bit, chld_bit, 0x3DF, 1);
}

/* ================================================================== *
 * Case: flags-mask — sa_mask / self-block / SA_NODEFER, both sides.
 * ================================================================== */

/* One delivery of `sig` under a guest raw action with `flags`/`sa_mask`; verify
 * the HOST-side mask the delivered handler observed. `pre_bit` is a bit the host
 * blocks before the call. */
static void mask_round(const char* tag, int sig, uint64_t flags, uint64_t sa_mask,
                       uint64_t pre_bit, int expect_autoblock) {
  uint64_t bm;
  uint64_t want = 0;
  uint64_t sig_bit = 1ull << (sig - 1);
  char buf[200];

  host_mask_clear();
  if (pre_bit) {
    int ps = __builtin_ffsll((long long)pre_bit);
    if (ps > 0) host_mask_block(ps);
  }

  bm = test_simple4(sym_nested_probe, (uint64_t)sig, flags, sa_mask, pre_bit);

  want |= 1ull << 0; /* handler ran */
  want |= 1ull << 1; /* right signal */
  if (expect_autoblock) want |= 1ull << 2;
  if (sa_mask) want |= 1ull << 3;
  if (pre_bit) want |= 1ull << 4;
  want |= 1ull << 5; /* nothing stray blocked inside */
  want |= 1ull << 6; /* same thread */
  want |= 1ull << 7; /* handler additions cleared after the return */
  if (pre_bit) want |= 1ull << 8;

  snprintf(buf, sizeof(buf), "%s: guest handler mask semantics", tag);
  check_bits(bm, want, buf);

  {
    uint64_t host_w = host_mask_word();
    if (pre_bit)
      check((host_w & pre_bit) == pre_bit,
            "host pre-call bit survived the delivery");
    check((host_w & (sig_bit | sa_mask)) == 0,
          "no handler-added bit leaked into the host mask");
  }
  host_mask_clear();
}

static void case_flags_mask(void) {
  /* 1. plain handler, empty sa_mask: auto-block inside, cleared after. */
  mask_round("plain", SIGUSR1, 0, 0, 0, 1);

  /* 2. SA_NODEFER: the delivered signal is NOT blocked inside. */
  mask_round("nodefer", SIGUSR1, (uint64_t)SA_NODEFER, 0, 0, 0);

  /* 3. explicit sa_mask (SIGALRM) plus a host bit set before the call. */
  mask_round("sa_mask", SIGUSR1, 0, 1ull << (SIGALRM - 1),
             1ull << (SIGUSR2 - 1), 1);

  /* 4. SA_NODEFER with sa_mask: only the explicit sa_mask bit is blocked. */
  mask_round("nodefer+sa_mask", SIGUSR1, (uint64_t)SA_NODEFER,
             1ull << (SIGALRM - 1), 0, 0);

  /* 5. The host's own view after the guest installs a nested-mask action: the
   *    engine must not have left the guest's bits blocked. */
  check(!host_mask_blocked(SIGALRM), "host mask has no SIGALRM after the rounds");
  check(!host_mask_blocked(SIGUSR1), "host mask has no SIGUSR1 after the rounds");

  /* 6. SA_SIGINFO changes the handler ARITY, not the mask semantics. */
  {
    uint64_t hb = test_simple0(sym_hits_b);
    check_u64(guest_action_set(SIGUSR1, GK_GUEST_B, 0, 0), 0,
              "SA_SIGINFO guest handler installed");
    check_u64(test_simple1(sym_raise, SIGUSR1), 0,
              "raise delivered to the SA_SIGINFO handler");
    check(test_simple0(sym_hits_b) > hb, "the 3-arg guest handler ran");
    check_u64((uint64_t)test_simple0(sym_handler_sig), SIGUSR1,
              "the SA_SIGINFO handler saw the right signal number");
  }
}

/* ================================================================== *
 * Case: altstack — SA_ONSTACK onto the guest's and the app's own alt stack.
 * ================================================================== */

static void case_altstack(void) {
  /* Guest side: the guest installs its OWN alt stack (a .bss buffer) and an
   * SA_ONSTACK handler; the handler's local must land inside that buffer, and
   * an unrelated local must not. */
  {
    uint64_t base = test_simple0(sym_altstack_base);
    uint64_t size = test_simple0(sym_altstack_size);
    check(base != 0 && size != 0, "the guest alt stack region is known");
    check_bits(test_simple1(sym_altstack_probe, SIGUSR1), 0x7,
               "guest SA_ONSTACK handler ran inside the guest alt stack");

    /* SS_ONSTACK must be clear again once the handler returned. */
    {
      char buf[32];
      check_u64(test_simple1(sym_altstack_get, (uint64_t)(uintptr_t)buf), 0,
                "guest sigaltstack() query succeeds after the delivery");
      check((((int*)buf)[2] & SS_ONSTACK) == 0,
            "SS_ONSTACK is cleared after the handler returned");
      check_u64((uint64_t)(((unsigned long*)buf)[2]), (uint64_t)size,
                "the guest alt stack size is reported back");
    }

    /* The probe deliberately leaves the guest alt stack installed, so the
     * query above observes the very registration the handler ran under. The
     * cleanup therefore belongs here, on the host, after those assertions. */
    check_u64(test_simple0(sym_altstack_disable), 0,
              "the host clears the guest alt stack after the delivery");
  }

  /* Native side: the APP's ONSTACK action, re-installed through the native
   * setter, must still land on the APP's alt stack. */
  {
    struct sigaction sa;
    struct sigaction nat;
    memset(&sa, 0, sizeof(sa));
    check(sigaction(SIGUSR1, NULL, &sa) == 0, "host can query the SIGUSR1 action");
    check((sa.sa_flags & SA_ONSTACK) != 0,
          "the engine kept the app's SA_ONSTACK in the kernel action");

    memset(&nat, 0, sizeof(nat));
    nat.sa_sigaction = app_handler;
    nat.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&nat.sa_mask);
    sigaddset(&nat.sa_mask, SIGUSR2);
    check(box64_signal_action_set_native(SIGUSR1, &nat, NULL) == 0,
          "native setter re-installed the app ONSTACK action");

    g_app_hits = 0;
    g_app_frame = 0;
    check_u64(test_simple1(sym_raise, SIGUSR1), 0,
              "real raise under the native action");
    check(g_app_hits >= 1, "the native ONSTACK handler ran");
    check(g_app_frame == 1,
          "the native handler's frame is inside the APP's alt stack");
  }
}

/* ================================================================== *
 * Case: restart — SA_RESTART vs EINTR on a real blocked read.
 * ================================================================== */

struct restart_arg {
  int fd;
  int flags;
  int guest;
  atomic_int ready; /* 1: installed and unblocked; -1: setup failed */
  atomic_int done;
  int64_t result;   /* read after joining the reader */
};

/* READY is published only after installing the action and unblocking SIGUSR2.
 * The driver sends signals to this thread before releasing the pipe. */
static void* restart_reader(void* p) {
  struct restart_arg* a = (struct restart_arg*)p;
  char buf[64];
  a->result = -99;
  if (a->guest) {
    if (box64_ensure_emu() != 0) goto failed;
    uint64_t r = test_simple2(sym_restart_install, SIGUSR2, (uint64_t)a->flags);
    if ((int64_t)r != 0) {
      a->result = -1000 + (int64_t)r;
      goto failed;
    }
  } else {
    sigset_t one;
    sigemptyset(&one);
    sigaddset(&one, SIGUSR2);
    if (pthread_sigmask(SIG_UNBLOCK, &one, NULL) != 0) goto failed;
  }
  atomic_store_explicit(&a->ready, 1, memory_order_release);
  if (a->guest) {
    a->result = (int64_t)test_simple3(sym_read_fd, (uint64_t)a->fd,
                                    (uint64_t)(uintptr_t)buf, sizeof(buf));
  } else {
    ssize_t n = read(a->fd, buf, sizeof(buf));
    a->result = n < 0 ? -errno : n;
  }
  atomic_store_explicit(&a->done, 1, memory_order_release);
  return NULL;
failed:
  atomic_store_explicit(&a->ready, -1, memory_order_release);
  atomic_store_explicit(&a->done, 1, memory_order_release);
  return NULL;
}

/* Start, signal the reader, release its pipe, then join exactly once.
 * run_bounded() cannot drive this test: it joins before its caller can signal. */
static void restart_round(const char* tag, int flags, int guest, int expect_eintr) {
  int fds[2];
  pthread_t thread;
  struct restart_arg a = {.flags = flags, .guest = guest, .result = -999};
  sigset_t one, saved;
  char msg[200];
  atomic_init(&a.ready, 0);
  atomic_init(&a.done, 0);
  if (pipe(fds) != 0) {
    check(0, "pipe() for the restart case");
    return;
  }
  a.fd = fds[0];
  sigemptyset(&one);
  sigaddset(&one, SIGUSR2);
  if (pthread_sigmask(SIG_BLOCK, &one, &saved) != 0) {
    check(0, "restart: save and block the driving thread's signal mask");
    close(fds[0]);
    close(fds[1]);
    return;
  }
  int rc = pthread_create(&thread, NULL, restart_reader, &a);
  snprintf(msg, sizeof(msg), "%s: reader thread started (rc=%d)", tag, rc);
  check(rc == 0, msg);
  if (rc != 0) {
    close(fds[0]);
    close(fds[1]);
    check(pthread_sigmask(SIG_SETMASK, &saved, NULL) == 0,
          "restart: restore the driving thread's mask");
    return;
  }

  for (int tries = 0; tries < 200 &&
       atomic_load_explicit(&a.ready, memory_order_acquire) == 0; ++tries)
    usleep(10 * 1000);
  int ready = atomic_load_explicit(&a.ready, memory_order_acquire);
  snprintf(msg, sizeof(msg), "%s: reader installed the action and unblocked SIGUSR2", tag);
  check(ready == 1, msg);
  if (ready == 1) {
    /* READY precedes read entry. Repeated targeted signals cover that small
     * scheduling window; the main thread never receives these deliveries. */
    usleep(150 * 1000);
    int sent_ok = 1;
    for (int tries = 0; tries < 20; ++tries) {
      if (atomic_load_explicit(&a.done, memory_order_acquire)) break;
      if (pthread_kill(thread, SIGUSR2) != 0) {
        sent_ok = 0;
        break;
      }
      usleep(50 * 1000);
    }
    snprintf(msg, sizeof(msg), "%s: SIGUSR2 was sent to the reader", tag);
    check(sent_ok, msg);
    if (!expect_eintr) {
      snprintf(msg, sizeof(msg), "%s: read stays blocked after signals, before a byte", tag);
      check(!atomic_load_explicit(&a.done, memory_order_acquire), msg);
    }
    if (!guest)
      check(atomic_load_explicit(&g_app_rt_hits, memory_order_relaxed) > 0,
            "SA_RESTART: the real HOST reader received its native handler before the byte");
  }

  /* Failed assertions also release the pipe. EOF handles a failed write, and
   * the read fd and stack argument stay alive until the reader has joined. */
  check(write(fds[1], "z", 1) == 1, "restart: release the reader with one byte");
  close(fds[1]);
  struct timespec deadline;
  clock_gettime(CLOCK_REALTIME, &deadline);
  deadline.tv_sec += 6;
  rc = pthread_timedjoin_np(thread, NULL, &deadline);
  snprintf(msg, sizeof(msg), "%s: reader joined after pipe release (rc=%d)", tag, rc);
  check(rc == 0, msg);
  if (rc != 0) {
    /* Never return and abandon a live reader's stack argument. */
    fflush(NULL);
    _exit(1);
  }
  close(fds[0]);
  check(pthread_sigmask(SIG_SETMASK, &saved, NULL) == 0,
        "restart: restore the driving thread's mask");
  snprintf(msg, sizeof(msg), "%s: read(2) result (got=%lld)", tag,
           (long long)a.result);
  check(expect_eintr ? a.result == -EINTR : a.result > 0, msg);
  if (guest)
    check(test_simple0(sym_hits_a) > 0, "restart: the guest signal handler really ran");
}

static void case_restart(void) {
  restart_round("no-restart", 0, 1, 1);
  restart_round("restart", SA_RESTART, 1, 0);

  struct sigaction sa, cur;
  box64_signal_action_t logical;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = app_restart_handler;
  sa.sa_flags = SA_RESTART;
  sigemptyset(&sa.sa_mask);
  check(box64_signal_action_set_native(SIGUSR2, &sa, NULL) == 0,
        "native setter installed the app restart handler");
  memset(&cur, 0, sizeof(cur));
  check(sigaction(SIGUSR2, NULL, &cur) == 0, "host query of the SIGUSR2 action");
  check((cur.sa_flags & SA_RESTART) != 0,
        "SA_RESTART is still set in the kernel action");
  memset(&logical, 0, sizeof(logical));
  check(box64_signal_action_get(SIGUSR2, &logical) == 0,
        "query the native action's logical flags");
  check(logical.kind == BOX64_SIGNAL_ACTION_NATIVE &&
        logical.handler == (uintptr_t)app_restart_handler &&
        ((uint32_t)logical.flags & SA_SIGINFO) == 0 &&
        ((uint32_t)logical.flags & SA_RESTART) != 0,
        "the logical native action keeps its 1-arg handler and SA_RESTART");
  check(cur.sa_handler != SIG_DFL && cur.sa_handler != SIG_IGN,
        "the kernel action is a real handler, not DEFAULT/IGNORE");
  atomic_store_explicit(&g_app_rt_hits, 0, memory_order_relaxed);
  restart_round("native HOST restart", SA_RESTART, 0, 0);
}

/* ================================================================== *
 * Case: resethand — RESETHAND as a virtual one-shot.
 * ================================================================== */

static void case_resethand(void) {
  box64_signal_action_t act;
  uint64_t bm;
  struct sigaction cur;

  check_u64(guest_action_set(SIGUSR1, GK_GUEST_A, (uint64_t)SA_RESETHAND, 0),
            0, "guest installed a RESETHAND action");

  check(sigaction(SIGUSR1, NULL, &cur) == 0, "host can query the kernel action");
  check((cur.sa_flags & SA_RESETHAND) == 0,
        "the kernel sa_flags carries NO SA_RESETHAND (the reset is virtual)");
  check(cur.sa_sigaction != NULL && cur.sa_sigaction != (void*)SIG_DFL,
        "the kernel action is still the engine's dispatcher, not SIG_DFL");

  bm = test_simple2(sym_one_shot_probe, SIGUSR1, (uint64_t)SA_RESETHAND);
  /* Guest rt_sigaction observes the logical table, not the host kernel entry.
   * Check the persistent dispatcher separately through host sigaction below. */
  check((bm & 1ull) != 0, "the RESETHAND handler ran");
  check_u64(test_simple0(sym_hits_a), 1, "exactly one delivery, one handler run");
  check_bits(bm & ((1ull << 4) | (1ull << 5) | (1ull << 6)),
             (1ull << 4) | (1ull << 5),
             "guest rt_sigaction reports the one-shot's logical DEFAULT");
  struct sigaction after;
  memset(&after, 0, sizeof(after));
  check(sigaction(SIGUSR1, NULL, &after) == 0,
        "host queries the kernel action after the one-shot");
  check(after.sa_sigaction == cur.sa_sigaction &&
        (after.sa_flags & SA_SIGINFO) != 0 &&
        (after.sa_flags & SA_RESETHAND) == 0,
        "after the one-shot the engine's dispatcher is still installed");
  check((bm & (1ull << 3)) != 0, "the signal is unblocked after the delivery");

  /* RE-REGISTRATION: an old (already-fired) one-shot variant must NOT override a
   * NEW registration, and each installation must fire exactly once. This is the
   * race the "old variant must not override the new" rule is about, and it has
   * a deterministic form on the quiescent path too. */
  check_bits(test_simple1(sym_rereg, SIGUSR1), 0x1f,
             "RESETHAND re-registration: each variant fires exactly once, "
             "the old variant does not override the new registration");

  /* The same rule, with a REAL delivery under the new registration: the handler
   * that runs is the NEW one, not the stale variant. */
  check_u64(test_simple1(sym_oneshot_which, SIGUSR1), 1,
            "the delivery after re-registration ran the NEW handler");
  check(box64_signal_action_get(SIGUSR1, &act) == 0,
        "query after the re-registration delivery");
  check(act.kind == BOX64_SIGNAL_ACTION_DEFAULT,
        "and it consumed exactly its own one shot");

  /* The LOGICAL action is DEFAULT again: that is what the virtual reset did. */
  memset(&act, 0, sizeof(act));
  check(box64_signal_action_get(SIGUSR1, &act) == 0, "query after the one-shot");
  check(act.kind == BOX64_SIGNAL_ACTION_DEFAULT,
        "the logical action is DEFAULT after one RESETHAND delivery");

  /* A SECOND delivery is therefore fatal: run it in a child. */
  {
    int status = 0;
    check(run_child(child_raise_sigusr1, NULL, &status) == 0,
          "forked the second-delivery child");
    check(WIFSIGNALED(status) && WTERMSIG(status) == SIGUSR1,
          "the second delivery is fatal with the SIGUSR1 default disposition");
    check_u64(test_simple0(sym_alive), 0x5a17,
              "the parent's engine still runs after the child's fatal delivery");
  }
}

/* ================================================================== *
 * Case: native-c-fault — a HARDWARE FAULT raised by NATIVE C code while a guest
 * call is active, with the logical action being a GUEST handler. This is the
 * mirror of native-fault and the direction that can corrupt a C frame: if the
 * engine resumed by a guest longjmp it would unwind PAST the native callback.
 *
 * Asserted:
 *   - the native callback's own C frame survives: its stack cookie is intact
 *     after the fault and the callback RETURNS normally to its caller;
 *   - the guest handler really ran and repaired the page (through the guest
 *     syscall path, which reaches the host mprotect);
 *   - the value the callback read after the repair is visible, so control came
 *     back through the callback rather than jumping over it.
 *
 * Runs on a bounded worker so a wrong resume cannot hang the case.
 * ================================================================== */

struct nf_job2 {
  void* bridge;
  uint64_t page;
  uint64_t cookie;
};

/* The bridged native callback, defined in the HOST. It takes a cookie, touches
 * the poisoned page the guest armed, and verifies the cookie afterwards. If the
 * engine unwound past this frame the verification below would never run. */
static volatile sig_atomic_t g_cb_entered;
static volatile sig_atomic_t g_cb_returned;
static volatile sig_atomic_t g_cb_cookie_ok;
static volatile uint64_t g_cb_cookie;
static volatile sig_atomic_t g_cb_value_seen;

static uint64_t host_faulting_callback(uint64_t cookie, uint64_t page,
                                       uint64_t c, uint64_t d, uint64_t e,
                                       uint64_t f) {
  volatile uint64_t local_cookie = cookie;
  (void)c;
  (void)d;
  (void)e;
  (void)f;
  g_cb_entered = 1;
  g_cb_cookie = cookie;
  g_cb_cookie_ok = 0;

  /* The fault: the page is PROT_NONE. A GUEST call is active (the guest invoked
   * this bridged callback), so the engine may route it to the guest action. */
  *(volatile unsigned char*)(uintptr_t)page = 0x77;

  /* Reaching here at all IS the assertion that the C frame survived. */
  if (local_cookie == cookie) {
    g_cb_cookie_ok = 1;
    g_cb_returned = 1;
    g_cb_value_seen = (sig_atomic_t)*(volatile unsigned char*)(uintptr_t)page;
  }
  return g_cb_cookie_ok ? 1u : 0u;
}

/* The worker: the GUEST invokes the bridged callback, which faults.
 *
 * The callback takes (cookie, page, ...): BOTH values must reach it. Handing it
 * the cookie alone left `page` as an uninitialised second register, so the
 * deliberate store faulted on an address the guest repair handler has no page
 * to fix -- the case then exhausted its retry bound and died by the SIGSEGV
 * default disposition, which reads as an engine crash but is this driver
 * passing the wrong arguments. */
static void* nf_worker(void* p) {
  struct nf_job2* j = (struct nf_job2*)p;
  if (box64_ensure_emu() != 0) return (void*)(intptr_t)-99;
  return (void*)(intptr_t)test_simple3(sym_nf_invoke, (uint64_t)(uintptr_t)j->bridge,
                                       j->page, j->cookie);
}

static void native_c_fault_round(const char* tag, int chaining) {
  void* cb;
  uint64_t prep;
  struct nf_job2 job;
  struct worker w;
  char msg[200];
  int r;

  g_cb_entered = 0;
  g_cb_returned = 0;
  g_cb_cookie_ok = 0;
  g_cb_cookie = 0;
  g_cb_value_seen = 0;
  g_app_hits = 0;
  g_app_sig = 0;
  g_app_saw_siginfo = 0;
  g_host_segv_hits = 0;
  g_nf_native_uc_seen = 0;
  g_nf_native_pc_resolves = 0;
  g_nf_native_pc = 0;

  /* The chaining round chains to the action that is current when the guest
   * saves it, so the app's native action must actually BE current here. */
  if (chaining) readopt_app_segv_native();

  prep = test_simple0(chaining ? sym_nf_chain_prepare : sym_nf_prepare);
  snprintf(msg, sizeof(msg), "%s: armed the poisoned page and the guest handler", tag);
  check((int64_t)prep > 0, msg);
  if ((int64_t)prep <= 0) return;

  /* The callback the guest will call. Registering it as a guest-callable bridge
   * is what makes this a GUEST-initiated call whose callee then faults. */
  cb = box64_callback((void*)host_faulting_callback);
  snprintf(msg, sizeof(msg), "%s: the host callback was bridged", tag);
  check(cb != NULL, msg);
  if (!cb) return;

  job.bridge = cb;
  job.page = (uint64_t)test_simple0(sym_nf_page);
  job.cookie = 0xc0ffeeULL;

  r = run_bounded(&w, nf_worker, &job, 8000);
  snprintf(msg, sizeof(msg), "%s: the faulting guest->host call finished", tag);
  check(r == 0, msg);
  if (r != 0) return;

  /* 1. THE C FRAME SURVIVED. This is the whole point of the case. */
  check(g_cb_entered == 1, "native-c-fault: the native callback ran");
  check(g_cb_returned == 1,
        "native-c-fault: the native callback RETURNED normally (no guest longjmp "
        "skipped the C frame)");
  check(g_cb_cookie_ok == 1,
        "native-c-fault: the callback's stack cookie was intact across the fault");
  check_u64((uint64_t)g_cb_cookie, job.cookie,
            "native-c-fault: the cookie value was preserved");
  check((int)w.rc == 1, "native-c-fault: the callback's return value reached the guest");

  /* 2. The guest hander really ran and repaired the page. */
  check((uint64_t)test_simple0(sym_nf_hits) >= 1,
        "native-c-fault: the GUEST handler received the native fault");
  check((uint64_t)test_simple0(sym_nf_fixed) == 1,
        "native-c-fault: the guest handler repaired the page (raw mprotect)");
  check(g_cb_value_seen == (sig_atomic_t)0x77,
        "native-c-fault: the store landed, so control returned through the callback");
  check((uint64_t)test_simple0(sym_fault_hopeless) == 0,
        "native-c-fault: the guest handler repaired the page within its retry bound");

  if (chaining) {
    check((uint64_t)test_simple0(sym_nf_chain_hits) >= 1,
          "native-c-fault: the guest handler chained to the saved native action");
    check(!g_cb_returned || g_host_segv_hits >= 1,
          "native-c-fault: the chained NATIVE action really ran from the guest "
          "handler");
    /* WRONG-ROUTE CONTROL for this round. Here the host action MUST fire,
     * because the application itself chained into it; what must not happen is a
     * delivery nobody asked for. The guest counts every chain call it issues and
     * the host counts every invocation it receives, so equality says both
     * "every chained call really reached the native action" and "nothing else
     * reached it". */
    check((uint64_t)g_host_segv_hits == (uint64_t)test_simple0(sym_nf_chain_hits),
          "native-c-fault: the host action ran exactly once per chained call "
          "(no unintended wrong-route delivery)");
    check(g_app_sig == SIGSEGV,
          "native-c-fault: the chained native action saw the real signal number");
    /* BRIDGE UFIDELITY: the guest handler passed its GUEST ucontext through, and
     * the engine must bridge it so the native handler receives a REAL host
     * ucontext_t -- a faulting PC in this process's own code that dladdr()
     * resolves, not a guest address masquerading as one. */
    check(g_nf_native_uc_seen == 1,
          "native-c-fault: the chained native action received a ucontext");
    check(g_nf_native_pc_resolves == 1,
          "native-c-fault: that ucontext is a REAL NATIVE one (its PC resolves "
          "to a host module, not a guest address)");
    check(g_app_saw_siginfo == 1,
          "native-c-fault: the chained native action received a real siginfo_t");
  } else {
    /* The plain round must never reach the host action: the fault belongs to the
     * engine, and a delivery that landed on the app's host SIGSEGV handler would
     * be the wrong-route bug this module exists to remove. The chaining round
     * cannot carry this check (it invokes the host action on purpose), so it is
     * asserted here, per round, instead of only once for the whole case. */
    check(g_host_segv_hits == 0,
          "native-c-fault: no unintended delivery reached the host action (plain "
          "round)");
  }
}

static void case_native_c_fault(void) {
  /* Direction 1: the guest action is the repairing guest handler; the native C
   * frame must survive and return. */
  native_c_fault_round("native-c-fault", 0);

  /* Direction 2: bridge chaining. The guest handler chains to the native action
   * the guest saved BEFORE it replaced it, passing the delivered arguments, so
   * the native handler must receive a REAL native ucontext. Asserting the
   * ucontext is genuinely native is done in app_segv_host via g_app_frame. */
  native_c_fault_round("native-c-fault-chain", 1);
}

/* ================================================================== *
 * Case: smc — the GS=0 dynarec internal repair must not leak, and must not
 * consume an app RESETHAND action.
 * ================================================================== */

static void case_smc(void) {
  uint64_t bm;

  /* The app installed its SIGSEGV action (SA_SIGINFO|SA_RESETHAND) BEFORE init.
   * The engine's write-protection repair of the guest's RWX page must be
   * handled INSIDE the engine: the app action must not run (so no virtual
   * reset) and must never see the fault. */
  g_host_segv_hits = 0;
  check_true(sym_smc_probe != NULL, "the SMC probe entry exists");
  bm = test_simple0(sym_smc_probe);
  check_bits(bm, 0x7,
             "guest RWX page: first call 42, patched immediate 7, same page");
  check(g_host_segv_hits == 0,
        "the host SIGSEGV action never saw the engine's internal repair");

  /* The app's RESETHAND action is unchanged and still the logical action. */
  {
    box64_signal_action_t act;
    struct sigaction cur;
    memset(&act, 0, sizeof(act));
    check(box64_signal_action_get(SIGSEGV, &act) == 0, "query the SIGSEGV action");
    check(act.kind == BOX64_SIGNAL_ACTION_NATIVE,
          "the app's SIGSEGV action is still the logical action after the repair");
    check((act.flags & SA_RESETHAND) != 0,
          "RESETHAND is still part of the app action (not consumed internally)");
    check(sigaction(SIGSEGV, NULL, &cur) == 0, "host query of the SIGSEGV action");
    check((cur.sa_flags & SA_RESETHAND) == 0,
          "the kernel SIGSEGV action still carries no reset bit");
  }

  /* The engine repaired the guest's code page WITHOUT the action ever being
   * delivered: the guest must be able to run the patched code again, i.e. the
   * page is still executable and the engine still owns its protection. */
  {
    uint64_t bm2 = test_simple0(sym_smc_probe);
    check_bits(bm2, 0x7, "a second SMC round also reports 42 then 7");
    check(g_host_segv_hits == 0, "still no host SIGSEGV action for the repair");
  }

  /* A REGULAR RESETHAND delivery (on SIGUSR2, so the app's SIGSEGV action stays
   * available for the assertions above) must consume exactly one shot. */
  {
    box64_signal_action_t act;
    uint64_t bm3;
    check_u64(guest_action_set(SIGUSR2, GK_GUEST_A, (uint64_t)SA_RESETHAND, 0),
              0, "guest installed a RESETHAND action on SIGUSR2");
    bm3 = test_simple2(sym_one_shot_probe, SIGUSR2, (uint64_t)SA_RESETHAND);
    check((bm3 & 1ull) != 0, "the SIGUSR2 RESETHAND handler ran");
    memset(&act, 0, sizeof(act));
    check(box64_signal_action_get(SIGUSR2, &act) == 0,
          "query SIGUSR2 after the one-shot");
    check(act.kind == BOX64_SIGNAL_ACTION_DEFAULT,
          "a real regular delivery consumed the one-shot (virtual reset)");
  }
}

/* ================================================================== *
 * Case: pagefix — a guest handler repairs a page; the guest resumes at the
 * FAULTING instruction (no native-PC fiction).
 * ================================================================== */

struct fault_job {
  uint64_t expect;
};

/* The faulting store, on a worker with its own emu and a bounded join: a wrong
 * resume wedges that worker, and the case must report instead of hanging. */
static void* fault_worker(void* p) {
  struct fault_job* j = (struct fault_job*)p;
  if (box64_ensure_emu() != 0) return (void*)(intptr_t)-99;
  return (void*)(intptr_t)test_simple1(sym_fault_store, j->expect);
}

static void case_pagefix(void) {
  struct fault_job job;
  struct worker w;
  int r;

  /* The guest maps a PROT_READ page and installs its own repairing SIGSEGV
   * handler through the ACTION API, so the delivery goes through the engine's
   * guest dispatch and the handler runs as a real guest frame. */
  check(test_simple1(sym_fault_prepare, GK_PAGEFIX) > 0,
        "guest armed a read-only page and a guest SIGSEGV repair handler");

  job.expect = 0x5a;
  r = run_bounded(&w, fault_worker, &job, 6000);
  check(r == 0, "the faulting store finished (engine resumed the guest)");
  if (r == 0) {
    check_bits((uint64_t)(intptr_t)w.rc, 0xf,
               "guest resumed at the faulting store after the handler's repair");
  }

  /* The bounded-retry guard must NOT have tripped: an unrepairable page would
   * otherwise wedge the worker, and this is what names that reason. */
  check((uint64_t)test_simple0(sym_fault_hopeless) == 0,
        "pagefix: the handler repaired the page within its retry bound");

  /* The same recovery must work when the fault is taken on the MAIN thread,
   * which is what an embedder's own guest call does. */
  {
    struct fault_job j2;
    check(test_simple1(sym_fault_prepare, GK_PAGEFIX) > 0,
          "re-armed a read-only page for the main-thread fault");
    j2.expect = 0xa5;
    check_bits((uint64_t)(intptr_t)test_simple1(sym_fault_store, j2.expect), 0xf,
               "main-thread guest fault resumed at the faulting store");
  }
}

/* ================================================================== *
 * Case: native-fault — the GUEST faults, the LOGICAL action is NATIVE, and the
 * native handler gets a REAL native ucontext it can act on: it repairs the page
 * with the host mprotect() (the "mprotect recovery" shape) and the guest resumes
 * at the faulting instruction, with no native-PC fiction. It also proves the
 * handler can read the faulting PC and address out of the delivered ucontext,
 * i.e. that a native handler of a guest fault sees real values, not placeholders.
 * ================================================================== */

static volatile sig_atomic_t g_nf_hits;
static volatile sig_atomic_t g_nf_sig;
static volatile sig_atomic_t g_nf_pc_nonzero;
static volatile sig_atomic_t g_nf_addr_match;
static volatile sig_atomic_t g_nf_fixed;
static void* g_nf_fix_addr;
static size_t g_nf_fix_len;

/* Native SIGSEGV action for a GUEST fault: recover by making the faulting page
 * writable again, exactly the way a JIT/GC embedder does. The ucontext is the
 * host one (we are the host handler), so REG_RIP is the host PC running the
 * engine -- reported and asserted non-zero, never fabricated. */
static void native_guest_fault_handler(int sig, siginfo_t* info, void* ucv) {
  ucontext_t* uc = (ucontext_t*)ucv;
  void* pc = NULL;
  g_nf_sig = sig;
  g_nf_hits++;
#if defined(__loongarch64)
  pc = uc ? (void*)uc->uc_mcontext.__pc : NULL;
  (void)pc;
#elif defined(__x86_64__)
  pc = uc ? (void*)uc->uc_mcontext.gregs[REG_RIP] : NULL;
#else
  (void)uc;
#endif
  if (pc) g_nf_pc_nonzero++;
  if (info && g_nf_fix_addr && info->si_addr == g_nf_fix_addr)
    g_nf_addr_match++;
  if (g_nf_fix_addr && g_nf_fix_len) {
    if (mprotect(g_nf_fix_addr, g_nf_fix_len, PROT_READ | PROT_WRITE) == 0)
      g_nf_fixed = 1;
  }
}

struct nf_job {
  uint64_t expect;
};

/* Not used directly: the guest arms the page and the handler, the harness sends
 * the faulting store through the engine. */
static void case_native_fault(void) {
  struct sigaction sa;
  box64_signal_action_t act;

  /* 1. The logical action for SIGSEGV is NATIVE, adopted from the app. */
  memset(&act, 0, sizeof(act));
  check(box64_signal_action_get(SIGSEGV, &act) == 0, "query the SIGSEGV action");
  check(act.kind == BOX64_SIGNAL_ACTION_NATIVE,
        "the SIGSEGV action is the app's NATIVE action");

  /* 2. Remember the page the guest is about to fault on, and make sure the
   *    handler is the repairing native one. */
  {
    long page = test_simple1(sym_fault_prepare, -1 /*leave the action alone*/);
    long psz = sysconf(_SC_PAGESIZE);
    check(page > 0, "the guest armed a read-only page (no guest handler)");
    check(psz > 0, "sysconf(_SC_PAGESIZE) is usable");
    /* Repair exactly the page the guest mapped, with the REAL host page size:
     * hard-coding 4096 would silently under- or over-cover on a 16K/64K host
     * (this very machine runs 4K, but the LoongArch hosts do not have to). */
    g_nf_fix_addr = (void*)(uintptr_t)page;
    g_nf_fix_len = (size_t)(psz > 0 ? psz : 4096);
  }
  memset(&sa, 0, sizeof(sa));
  sa.sa_sigaction = native_guest_fault_handler;
  sa.sa_flags = SA_SIGINFO;
  sigemptyset(&sa.sa_mask);
  check(box64_signal_action_set_native(SIGSEGV, &sa, NULL) == 0,
        "native setter installed the repairing SIGSEGV action");

  /* 3. Fault through the engine on a bounded worker, so a wrong resume cannot
   *    hang the case. */
  {
    struct fault_job job;
    struct worker w;
    int r;
    g_nf_hits = 0;
    g_nf_pc_nonzero = 0;
    g_nf_addr_match = 0;
    g_nf_fixed = 0;
    job.expect = 0x3c;
    r = run_bounded(&w, fault_worker, &job, 6000);
    check(r == 0, "native-fault: the faulting store finished");
    /* g_fault_store bits 0+3 prove store completion and value readback.
     * Bits 1+2 are GUEST-handler counters, so they must stay clear when the
     * NATIVE action repaired this fault (its own counters are checked below). */
    if (r == 0)
      check_bits((uint64_t)(intptr_t)w.rc, 0x9,
                 "native-fault: the guest resumed AT the faulting store");
    check(g_nf_hits >= 1, "native-fault: the NATIVE action received the GUEST fault");
    check(g_nf_fixed == 1, "native-fault: the handler repaired the page (mprotect)");
    check(g_nf_pc_nonzero >= 1,
          "native-fault: a REAL host PC was delivered in the ucontext");
    check(g_nf_addr_match >= 1,
          "native-fault: si_addr is the real faulting guest address");
  }

  /* 4. The same fault must ALSO be recoverable by a GUEST handler with no
   *    native-PC fiction (the pagefix case covers the plain form); here the
   *    point is that the native action and the guest action disagree only about
   *    WHO runs, not about what the fault looks like. */
  {
    long page = test_simple1(sym_fault_prepare, GK_PAGEFIX);
    check(page > 0, "native-fault: re-armed with a GUEST repair handler");
    check_bits((uint64_t)(intptr_t)test_simple1(sym_fault_store, 0xc3), 0xf,
               "native-fault: the GUEST handler recovered the same fault shape");
  }
}

/* ================================================================== *
 * Case: chld — SIGCHLD NOCLDSTOP/NOCLDWAIT semantics.
 * ================================================================== */

static void case_chld(void) {
  uint64_t bm;
  box64_signal_action_t act;
  uint64_t a0 = (uint64_t)SA_NOCLDWAIT;

  check_u64(guest_action_set(SIGCHLD, GK_CHLD, 0, 0), 0,
            "guest installed a SIGCHLD handling action");

  /* The logical query reports the flags the app set, not box64's kernel
   * arming. Put NOCLDWAIT on the action first so the query can see it. */
  check_u64(guest_action_set(SIGCHLD, GK_CHLD, a0, 0), 0,
            "guest set a SIGCHLD action with SA_NOCLDWAIT");
  memset(&act, 0, sizeof(act));
  check(box64_signal_action_get(SIGCHLD, &act) == 0, "query the SIGCHLD action");
  check((act.flags & SA_NOCLDWAIT) != 0,
        "the logical SIGCHLD action carries SA_NOCLDWAIT");
  check(act.kind == BOX64_SIGNAL_ACTION_GUEST, "SIGCHLD action kind is GUEST");

  /* NOCLDWAIT: the child is auto-reaped, so wait4 must report ECHILD and the
   * SIGCHLD handler still runs.
   *
   * The bit values are the FIXTURE's contract, spelled out in
   * tests/guest/signal_actions_guest.c at g_sigchld_probe():
   *   1 = fork produced a child, 2 = wait4 returned -ECHILD (auto-reaped),
   *   4 = wait4 returned the child's pid (normal reap), 8 = the handler ran.
   * They are named here as constants so the two sides cannot drift apart
   * silently again — this case had never been reached by the matrix. */
  enum { CHLD_FORKED = 1, CHLD_ECHILD = 2, CHLD_REAPED = 4, CHLD_HANDLER = 8 };
  bm = test_simple1(sym_sigchld_probe, a0);
  check((bm & CHLD_FORKED) != 0, "SIGCHLD NOCLDWAIT: the child forked");
  check((bm & CHLD_ECHILD) != 0,
        "SA_NOCLDWAIT: wait4 reported ECHILD (the child was auto-reaped)");
  check((bm & CHLD_HANDLER) != 0,
        "the SIGCHLD handler still ran under NOCLDWAIT");

  /* Without NOCLDWAIT the child is reaped normally. */
  check_u64(guest_action_set(SIGCHLD, GK_CHLD, (uint64_t)SA_NOCLDSTOP, 0),
            0, "guest set a SIGCHLD action with only SA_NOCLDSTOP");
  bm = test_simple1(sym_sigchld_probe, (uint64_t)SA_NOCLDSTOP);
  check((bm & CHLD_FORKED) != 0, "SIGCHLD: the child forked");
  check((bm & CHLD_REAPED) != 0,
        "without NOCLDWAIT the child is reaped normally (wait4 returns its pid)");

  /* SIGCHLD is a legal action target and must never be treated as fatal. */
  check(box64_signal_action_get(SIGCHLD, &act) == 0,
        "SIGCHLD is a valid action target");
}

/* ================================================================== *
 * Case: longjmp — the CHANGED-GUEST-CONTEXT resume path, asserted as such.
 *
 * uc-mask proves the handler's mask decision survives; this case proves the
 * delivery actually took the siglongjmp resume (rather than a plain return),
 * which is what makes that mask restoration interesting in the first place.
 * The discriminator is the engine's own counter: on the FAST policy every
 * signal-driven resume that restores the mask bumps box64_signal_stats()'s
 * mask_restores; a plain return bumps nothing. Under COMPAT the same delivery
 * must still preserve the handler's uc_sigmask edit, but the restore is done by
 * siglongjmp itself, so only the FAST run may see the counter move.
 * ================================================================== */

static void case_longjmp(void) {
  box64_signal_stats_t before, after;
  uint64_t alarm_bit = 1ull << (SIGALRM - 1);
  uint64_t bm;
  int fast = (box64_signal_policy_get() == BOX64_SIGNAL_POLICY_FAST);
  uint64_t host_w;

  box64_signal_stats_enable(1);

  /* The COMBINED edit (mode 4: R11 XOR plus uc_sigmask OR) is what makes this
   * case definitive. A mask-only edit is NOT required to resume by siglongjmp --
   * the engine may legally copy it onto the native ucontext and return normally
   * -- so requiring a counter bump for a mask-only edit would be asserting old
   * implementation behaviour rather than the frozen contract. Changing the
   * emulator-visible REGISTER state as well puts the delivery on the
   * changed-state path for real. */
  host_mask_clear();
  memset(&before, 0, sizeof(before));
  box64_signal_stats(&before);
  bm = test_simple6(sym_uc_edit, SIGUSR1, 0, 4 /*combined*/, alarm_bit, 0, 0);
  memset(&after, 0, sizeof(after));
  box64_signal_stats(&after);

  /* SEMANTIC requirement, identical under both policies: the handler ran and
   * the mask its UCONTEXT edit asked for survived. Bit 4 of the post-delivery
   * half says the uc_mask bits are BLOCKED after the delivery. */
  check((bm & 1ull) != 0, "longjmp: the combined-edit handler ran");
  check((bm & (1ull << 4)) != 0,
        "longjmp: the uc_sigmask edit survived (combined edit)");
  host_w = host_mask_word();
  check((host_w & alarm_bit) == alarm_bit,
        "longjmp: the host mask carries the handler's edited bit");
  check((host_w & (1ull << (SIGUSR1 - 1))) == 0,
        "longjmp: the auto-block bit is gone");

  if (fast) {
    /* FAST-only: the changed state really took the signal-driven resume and ran
     * the restore helper. */
    check(after.mask_restores > before.mask_restores,
          "longjmp: FAST took the signal-driven resume and ran the restore helper");
    check(after.setjmp_skips > 0, "longjmp: FAST frames are armed with savemask=0");
  } else {
    /* COMPAT restores by siglongjmp itself, so the FAST helper must not run --
     * but the mask outcome above is still required. */
    check(after.mask_restores == before.mask_restores,
          "longjmp: COMPAT resumes through siglongjmp, not the FAST helper");
    check(after.setjmp_saves > 0, "longjmp: COMPAT frames save the entry mask");
  }

  /* The register-only edit (mode 3) is the second form that changes emulator
   * state with no mask change at all. */
  memset(&before, 0, sizeof(before));
  box64_signal_stats(&before);
  bm = test_simple6(sym_uc_edit, SIGUSR1, 0, 3 /*R11 only*/, 0, 0, 0);
  memset(&after, 0, sizeof(after));
  box64_signal_stats(&after);
  check((bm & 1ull) != 0, "longjmp: the R11-perturbation handler ran");
  if (fast)
    check(after.mask_restores > before.mask_restores,
          "longjmp: the register-only edit also took the resume path");

  /* A NULL edit must not resume: that is what makes the counters above
   * meaningful rather than constant. */
  memset(&before, 0, sizeof(before));
  box64_signal_stats(&before);
  bm = test_simple6(sym_uc_edit, SIGUSR1, 0, 0 /*no edit*/, 0, 0, 0);
  memset(&after, 0, sizeof(after));
  box64_signal_stats(&after);
  check((bm & 1ull) != 0, "longjmp: the no-edit handler ran");
  check(after.mask_restores == before.mask_restores,
        "longjmp: an UNCHANGED context does not take the resume path");
  host_mask_clear();
}

/* ------------------------------------------------------------------ *
 * The two RACE cases deliberately use a DEFAULT-IGNORE signal, and this is a
 * CORRECTNESS requirement, not a style choice.
 *
 * Both races install a SA_RESETHAND action and then fire the signal from another
 * thread in a loop. Between the moment a one-shot is CONSUMED and the moment the
 * next registration lands, the logical action is DEFAULT. A delivery in that
 * window is therefore a perfectly LEGAL default delivery — and for a
 * default-TERMINATING signal that means the process dies. Attributing that to an
 * engine failure would be flatly wrong: the engine would have behaved correctly
 * and the case would report a bug that does not exist.
 *
 * SIGWINCH (default IGNORE on Linux, and the same number in the guest) closes
 * that window: a delivery while the action is DEFAULT is simply discarded, so the
 * race can fire as fast as it likes without a legal fatal exit. SIGWINCH is
 * also free of the SIGCHLD-style side effects (no auto-reap, no child state), so
 * it cannot perturb the process in any other way.
 *
 * The cases are SCOPED: RR_SIG for resethand-race, RACE_SIG for concurrent.
 * Every re-registration, fire, query and handler expectation in those two cases
 * goes through its scoped constant. No other case is affected -- in particular
 * the uc-mask / flags-mask / longjmp / fatal cases keep SIGUSR1 exactly as they
 * are, because they never fire into a DEFAULT window.
 * ------------------------------------------------------------------ */

#define RR_SIG SIGWINCH
#define RACE_SIG SIGWINCH

/* ================================================================== *
 * Case: resethand-race — concurrent RESETHAND re-registration while real
 * deliveries happen. The old (already-fired) one-shot variant must never
 * override a newer registration, and each installation must fire exactly once.
 * ================================================================== */

struct rr_ctx {
  _Atomic int stop;
  _Atomic int bad;       /* a failure that is NOT a documented refusal */
  _Atomic int refused;   /* transient trylock or bounded-pool refusal */
  _Atomic long delivered;
  _Atomic long registrations;
};

/* Fires RR_SIG until the flag is set; used only from the racing thread. Every
 * delivery that lands must be attributed to a current registration, which the
 * guest side checks for exactly-once. A delivery landing while the one-shot has
 * been consumed is a legal DEFAULT delivery and is simply ignored (RR_SIG's
 * default), never fatal. */
static void* rr_firer(void* p) {
  struct rr_ctx* c = (struct rr_ctx*)p;
  int i;
  for (i = 0; i < 300 && !atomic_load_explicit(&c->stop, memory_order_relaxed); ++i) {
    kill(getpid(), RR_SIG);
    atomic_fetch_add_explicit(&c->delivered, 1, memory_order_relaxed);
    usleep(500);
  }
  return NULL;
}

static void* rr_registrar(void* p) {
  struct rr_ctx* c = (struct rr_ctx*)p;
  int i = 0;
  while (!atomic_load_explicit(&c->stop, memory_order_relaxed) && i < 300) {
    int64_t r = (int64_t)test_simple1(sym_rereg_one, RR_SIG);
    /* TWO documented, non-corrupting refusals are expected under concurrency:
     *   -EWOULDBLOCK  the writer mutex is held (returned BEFORE any state change)
     *   -ENOMEM       the bounded preallocated node pool is exhausted
     * Both are the module behaving as specified under load, so neither is a hard
     * failure. The guest returns the refusal code itself (it cannot publish the
     * engine's errno through the raw syscall), so the host compares the RETURN
     * VALUE, not its own errno. */
    if (r == 0)
      atomic_fetch_add_explicit(&c->registrations, 1, memory_order_relaxed);
    else if (r == -EWOULDBLOCK || r == -ENOMEM)
      atomic_fetch_add_explicit(&c->refused, 1, memory_order_relaxed);
    else
      atomic_fetch_add_explicit(&c->bad, 1, memory_order_relaxed);
    i++;
  }
  return NULL;
}

static void case_resethand_race(void) {
  struct rr_ctx c;
  pthread_t firer, reg;
  int started = 0;

  memset(&c, 0, sizeof(c));
  host_mask_clear();
  /* RR_SIG must be deliverable to whichever thread can take it. */
  host_mask_unblock(RR_SIG);

  if (pthread_create(&reg, NULL, rr_registrar, &c) != 0) {
    check(0, "resethand-race: registrar started");
    return;
  }
  started++;
  if (pthread_create(&firer, NULL, rr_firer, &c) != 0) {
    check(0, "resethand-race: firer started");
    atomic_store(&c.stop, 1);
    pthread_join(reg, NULL);
    return;
  }
  started++;
  pthread_join(firer, NULL);
  atomic_store(&c.stop, 1);
  pthread_join(reg, NULL);
  (void)started;

  check(atomic_load(&c.bad) == 0,
        "resethand-race: no registration failed for an undocumented reason "
        "(bounded trylock/pool refusals are expected and allowed)");
  check(atomic_load(&c.registrations) > 0,
        "resethand-race: at least one registration succeeded");
  check(atomic_load(&c.registrations) + atomic_load(&c.refused) +
            atomic_load(&c.delivered) > 0,
        "resethand-race: the race really ran");
  if (atomic_load(&c.refused))
    printf("[signal-actions]      note: %d bounded trylock/pool refusals during "
           "the race (allowed)\n", atomic_load(&c.refused));

  /* QUIESCENT: with the race over and the writers stopped, a registration MUST
   * succeed (no permanent refusal) and a single delivery must run the NEW
   * handler exactly once. A stale one-shot variant overriding it would show
   * here. Same scoped signal, so nothing else is perturbed. */
  check_u64(test_simple1(sym_oneshot_which, RR_SIG), 1,
            "resethand-race: after the race the NEW registration ran");
  check_bits(test_simple1(sym_rereg, RR_SIG), 0x1f,
             "resethand-race: exactly-once per registration still holds");
}

/* ================================================================== *
 * Case: concurrent — coherent snapshots under controlled native updates,
 * concurrent queries and real deliveries. Run under MALLOC_PERTURB_.
 * ================================================================== */

static void race_handler(int sig, siginfo_t* info, void* ucv);
static void race_variant_b(int sig, siginfo_t* info, void* ucv);

/* A controlled native update the writers publish, and the EXACT tuple a reader
 * must see. The point of the coherence check is not "kind is known and the
 * handler is non-zero" (a torn read can satisfy that): a reader must observe one
 * of these published (handler, flags, mask) COMBINATIONS, never a mix of two. */
struct race_variant {
  void (*handler)(int, siginfo_t*, void*);
  int siginfo;
  uint64_t flags;
  int mask_sig; /* 0 = empty sa_mask */
};

/* The published states the writers cycle through. SA_RESETHAND is included on
 * purpose: it is a legitimate state, and with RACE_SIG being DEFAULT-IGNORE its
 * "consumed, now DEFAULT" window is harmless (a delivery there is discarded,
 * not fatal). With a default-TERMINATING signal this variant plus concurrent
 * firing would produce a LEGAL fatal exit that the case would misreport. */
static const struct race_variant g_race_variants[] = {
  { race_handler, 1, SA_SIGINFO, 0 },
  { race_handler, 1, SA_SIGINFO | SA_NODEFER, SIGALRM },
  { race_handler, 1, SA_SIGINFO | SA_ONSTACK, 0 },
  { race_handler, 1, SA_SIGINFO | SA_RESTART, SIGUSR2 },
  { race_variant_b, 1, SA_SIGINFO | SA_RESETHAND, 0 },
};
#define RACE_NV ((int)(sizeof(g_race_variants) / sizeof(g_race_variants[0])))

/* Every counter shared between threads is a C11 _Atomic object: `volatile` on a
 * plain int/long is NOT a data race proof, it only stops the compiler from
 * caching the value in a register. */
struct race_ctx {
  _Atomic int stop;
  _Atomic int bad;      /* a really incoherent snapshot, or a hard failure */
  _Atomic int hard;     /* a setter failure that is NOT a transient trylock and
                         * NOT a resource bound: a genuine bug */
  _Atomic int enomem;   /* the bounded node pool was exhausted: allowed per the
                         * header's errno contract, and NOT a corruption signal */
  _Atomic int refused;  /* a TRANSIENT EWOULDBLOCK from the setter's trylock */
  _Atomic long readers;
  _Atomic long writers;
  _Atomic long deliveries;
  _Atomic uint64_t gen_min;
  _Atomic uint64_t gen_max;
};

static _Atomic sig_atomic_t g_race_ok;

static void race_handler(int sig, siginfo_t* info, void* ucv) {
  atomic_store_explicit(&g_race_ok, sig, memory_order_relaxed);
  (void)info;
  (void)ucv;
}

static void race_variant_b(int sig, siginfo_t* info, void* ucv) {
  atomic_store_explicit(&g_race_ok, sig, memory_order_relaxed);
  (void)info;
  (void)ucv;
}

/* Publish variant i and return 0, -1 with errno EWOULDBLOCK, or -1 with any
 * other errno. The distinction matters: a trylock refusal is a transient,
 * EXPECTED outcome under concurrency, while a failure of another errno is a
 * real bug and must not be laundered into the "refused" counter. */
static int race_publish(int i) {
  struct sigaction sa;
  const struct race_variant* v = &g_race_variants[i % RACE_NV];
  memset(&sa, 0, sizeof(sa));
  sa.sa_sigaction = v->handler;
  sa.sa_flags = (int)v->flags;
  sigemptyset(&sa.sa_mask);
  if (v->mask_sig) sigaddset(&sa.sa_mask, v->mask_sig);
  if (box64_signal_action_set_native(RACE_SIG, &sa, NULL) == 0) return 0;
  /* Two documented, non-corrupting refusals, per the header's ERRNO CONTRACT:
   *   EWOULDBLOCK  the writer mutex is held; returned BEFORE any state changes
   *   ENOMEM       the bounded preallocated node supply is exhausted
   * Both are the module behaving exactly as specified under load. Anything else
   * is a real failure. Neither can leave a half-published snapshot: the header
   * states both refusals happen before any state is touched and that a one-shot
   * consumption is a single CAS. This case keeps the STRICT assertion -- a hard
   * error fails the case -- so a real bug cannot hide behind a resource
   * excuse. */
  if (errno == EWOULDBLOCK) return -1;
  if (errno == ENOMEM) return -3;
  return -2;
}

static void race_note_gen(struct race_ctx* c, uint64_t g) {
  uint64_t cur = atomic_load_explicit(&c->gen_min, memory_order_relaxed);
  while ((cur == 0 || g < cur) &&
         !atomic_compare_exchange_weak_explicit(&c->gen_min, &cur, g,
                                                memory_order_relaxed,
                                                memory_order_relaxed)) {
  }
  cur = atomic_load_explicit(&c->gen_max, memory_order_relaxed);
  while (g > cur &&
         !atomic_compare_exchange_weak_explicit(&c->gen_max, &cur, g,
                                                memory_order_relaxed,
                                                memory_order_relaxed)) {
  }
}

/* Exact sigset_t equality over the complete valid signal range (1..64). A
 * "the published bit is present" test is NOT enough: it silently accepts extra
 * bits the variant never published. */
static int race_mask_equal(const sigset_t* got, const sigset_t* want) {
  int s;
  for (s = 1; s <= 64; ++s) {
    if ((sigismember(got, s) == 1) != (sigismember(want, s) == 1)) return 0;
  }
  return 1;
}

/* Does this snapshot match exactly one published variant? The case installs its
 * own CONTROLLED action with box64_signal_action_set_native() from a memset
 * struct before any thread starts, so no snapshot a reader can observe is
 * inherited from a pre-init kernel action and no SA_RESTORER allowance applies:
 * every observable combination is one of g_race_variants EXACTLY. The initial
 * action the case installs is byte-identical to variant 0. */
static int race_snapshot_matches(const box64_signal_action_t* a) {
  int i;
  for (i = 0; i < RACE_NV; ++i) {
    const struct race_variant* v = &g_race_variants[i];
    sigset_t want;
    if (a->handler != (uintptr_t)v->handler) continue;
    /* The EXACT published logical flags, widened unsigned (the file header's
     * (uint64_t)(uint32_t) rule: SA_RESETHAND is 0x80000000). Nothing is masked
     * off -- the module records verbatim what the caller passed -- and no extra
     * bit is tolerated. */
    if (a->flags != (uint64_t)(uint32_t)v->flags) continue;
    /* The controlled update expresses an EMPTY restorer: the memset struct
     * carries sa_restorer == 0 and the logical action records that verbatim. */
    if (a->restorer != 0) continue;
    /* EXACT logical mask: empty for the empty variants, exactly the one
     * published bit otherwise. */
    sigemptyset(&want);
    if (v->mask_sig) sigaddset(&want, v->mask_sig);
    if (!race_mask_equal(&a->mask, &want)) continue;
    return 1;
  }
  return 0;
}

/* The OTHER state this case can legitimately observe: the clean DEFAULT node
 * the module arms as the one-shot successor at consumption. src arm_reset()
 * memsets the whole snapshot and sets only kind = DEFAULT (and a newer
 * generation), so handler, restorer and the whole logical flag word are zero and
 * the mask is EMPTY. The generation is deliberately NOT constrained here: it is
 * a fresh reserved number, so it is bounded only by the case's separate
 * monotone-generation check. */
static int race_snapshot_is_clean_default(const box64_signal_action_t* a) {
  sigset_t empty;
  if (a->handler != 0) return 0;
  if (a->restorer != 0) return 0;
  if (a->flags != 0) return 0;
  sigemptyset(&empty);
  if (!race_mask_equal(&a->mask, &empty)) return 0;
  return 1;
}

static void* race_writer(void* p) {
  struct race_ctx* c = (struct race_ctx*)p;
  int i = 0;
  while (!atomic_load_explicit(&c->stop, memory_order_relaxed) && i < 4000) {
    int r = race_publish(i);
    if (r == 0)
      atomic_fetch_add_explicit(&c->writers, 1, memory_order_relaxed);
    else if (r == -1)
      atomic_fetch_add_explicit(&c->refused, 1, memory_order_relaxed);
    else if (r == -3)
      atomic_fetch_add_explicit(&c->enomem, 1, memory_order_relaxed);
    else
      atomic_fetch_add_explicit(&c->hard, 1, memory_order_relaxed);
    i++;
  }
  return NULL;
}

static void* race_reader(void* p) {
  struct race_ctx* c = (struct race_ctx*)p;
  int i = 0;
  uint64_t prev_gen = 0; /* PER-THREAD: a monotone check against another
                          * thread's last observation would be meaningless */
  while (!atomic_load_explicit(&c->stop, memory_order_relaxed) && i < 200000) {
    box64_signal_action_t a;
    memset(&a, 0, sizeof(a));
    if (box64_signal_action_get(RACE_SIG, &a) == 0) {
      /* The ONLY states this case can publish are the controlled NATIVE
       * variants and, for the SA_RESETHAND variant, the clean DEFAULT successor
       * the module arms at consumption (src arm_reset(): memset to zero, kind
       * DEFAULT, handler/restorer 0, flags 0, empty mask). Anything else is a
       * state that was never published, so it is a failure rather than
       * something to wave through:
       *   GUEST  no guest rt_sigaction runs in this case at all;
       *   IGNORE no writer ever installs SIG_IGN. RACE_SIG is
       *          default-IGNORE by the KERNEL, which is DEFAULT behaviour for
       *          this signal, not kind == IGNORE. */
      if (a.kind == BOX64_SIGNAL_ACTION_NATIVE) {
        if (a.handler == 0) atomic_fetch_add_explicit(&c->bad, 1, memory_order_relaxed);
        /* EXACT published combination, not merely a plausible one. */
        else if (!race_snapshot_matches(&a))
          atomic_fetch_add_explicit(&c->bad, 1, memory_order_relaxed);
      } else if (a.kind == BOX64_SIGNAL_ACTION_DEFAULT) {
        if (!race_snapshot_is_clean_default(&a))
          atomic_fetch_add_explicit(&c->bad, 1, memory_order_relaxed);
      } else {
        /* GUEST, IGNORE, or a kind outside the defined set. */
        atomic_fetch_add_explicit(&c->bad, 1, memory_order_relaxed);
      }
      /* Monotone generation, checked against THIS thread's previous read. */
      if (prev_gen && a.generation < prev_gen)
        atomic_fetch_add_explicit(&c->bad, 1, memory_order_relaxed);
      prev_gen = a.generation;
      race_note_gen(c, a.generation);
      atomic_fetch_add_explicit(&c->readers, 1, memory_order_relaxed);
    } else {
      atomic_fetch_add_explicit(&c->bad, 1, memory_order_relaxed);
    }
    i++;
  }
  return NULL;
}

static void* race_firer(void* p) {
  struct race_ctx* c = (struct race_ctx*)p;
  int i;
  for (i = 0; i < 200 && !atomic_load_explicit(&c->stop, memory_order_relaxed); ++i) {
    int tries;
    /* A writer that is mid-update may transiently leave the signal blocked;
     * the delivery is then not taken yet, which is a retry, not a lost signal.
     * Bound the retry so the case still terminates. */
    atomic_store_explicit(&g_race_ok, 0, memory_order_relaxed);
    for (tries = 0; tries < 50; ++tries) {
      kill(getpid(), RACE_SIG);
      if (atomic_load_explicit(&g_race_ok, memory_order_relaxed) == RACE_SIG) {
        atomic_fetch_add_explicit(&c->deliveries, 1, memory_order_relaxed);
        break;
      }
      usleep(200);
    }
    usleep(1000);
  }
  return NULL;
}

static void case_concurrent(void) {
  enum { NW = 3, NR = 3 };
  struct race_ctx c;
  pthread_t wr[NW], rd[NR], fr;
  int i;

  memset(&c, 0, sizeof(c));
  host_mask_clear();
  host_mask_unblock(RACE_SIG);

  {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = race_handler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    check(box64_signal_action_set_native(RACE_SIG, &sa, NULL) == 0,
          "concurrent: the initial native action is installed");
  }

  for (i = 0; i < NR; ++i) {
    if (pthread_create(&rd[i], NULL, race_reader, &c) != 0) {
      check(0, "concurrent: reader started");
      atomic_store(&c.stop, 1);
      return;
    }
  }
  for (i = 0; i < NW; ++i) {
    if (pthread_create(&wr[i], NULL, race_writer, &c) != 0) {
      check(0, "concurrent: writer started");
      atomic_store(&c.stop, 1);
      return;
    }
  }
  if (pthread_create(&fr, NULL, race_firer, &c) != 0) {
    check(0, "concurrent: firer started");
    atomic_store(&c.stop, 1);
  } else {
    pthread_join(fr, NULL);
  }
  atomic_store(&c.stop, 1);
  for (i = 0; i < NW; ++i) pthread_join(wr[i], NULL);
  for (i = 0; i < NR; ++i) pthread_join(rd[i], NULL);

  check(atomic_load(&c.bad) == 0,
        "concurrent: every query returned a coherent snapshot");
  check(atomic_load(&c.hard) == 0,
        "concurrent: no controlled update failed for an undocumented reason");
  if (atomic_load(&c.enomem))
    printf("[signal-actions]      note: %d/%ld controlled updates hit the "
           "bounded node pool (ENOMEM, allowed by the header)\n",
           atomic_load(&c.enomem),
           atomic_load(&c.writers) + atomic_load(&c.refused) +
               atomic_load(&c.enomem) + atomic_load(&c.hard));
  check(atomic_load(&c.readers) > 0 && atomic_load(&c.writers) > 0,
        "concurrent: readers and writers really interleaved");
  check(atomic_load(&c.writers) + atomic_load(&c.refused) +
            atomic_load(&c.enomem) + atomic_load(&c.hard) > 0,
        "concurrent: every controlled update either applied or was refused");
  check(atomic_load(&c.deliveries) > 0,
        "concurrent: real deliveries landed in the racing handler");
  check(atomic_load(&c.gen_max) >= atomic_load(&c.gen_min) &&
            atomic_load(&c.gen_max) > 0,
        "concurrent: the generation only advances and is published");
  if (atomic_load(&c.refused))
    printf("[signal-actions]      note: %d/%ld controlled updates got a transient "
           "trylock refusal (allowed)\n", atomic_load(&c.refused),
           atomic_load(&c.writers) + atomic_load(&c.refused));

  /* After the race quiesces, a fresh update must succeed and be observable, so
   * a permanent refusal cannot hide behind the transient ones. */
  {
    box64_signal_action_t a;
    check(race_publish(0) == 0,
          "concurrent: a quiescent update succeeds (no permanent refusal)");
    memset(&a, 0, sizeof(a));
    check(box64_signal_action_get(RACE_SIG, &a) == 0,
          "concurrent: quiescent query");
    check(a.kind == BOX64_SIGNAL_ACTION_NATIVE &&
              a.handler == (uintptr_t)race_handler,
          "concurrent: the quiescent update is observable");
  }
  check((uint64_t)g_host_segv_hits == 0,
        "concurrent: no host SIGSEGV action fired during the race");
}

/* ================================================================== *
 * Case: fatal — a DEFAULT disposition is preserved (terminal re-delivery only).
 * ================================================================== */

static void case_fatal(void) {
  int status = 0;

  check(run_child(child_raise_sigusr1, NULL, &status) == 0,
        "forked the SIGUSR1 child");
  check(WIFSIGNALED(status), "the DEFAULT SIGUSR1 child died of a signal");
  if (WIFSIGNALED(status))
    check_u64((uint64_t)WTERMSIG(status), SIGUSR1,
              "the DEFAULT SIGUSR1 disposition really terminates the child");

  /* The point of "terminal only": the fatal delivery happened in a child and
   * did not take the parent's engine or action table with it. */
  check_u64(test_simple0(sym_alive), 0x5a17,
            "the parent's engine still runs after the child's fatal delivery");

  /* A real hardware fault with SIGSEGV at DEFAULT. The LOGICAL action is
   * DEFAULT for exactly this signal: child_default_segv performs a real host
   * hardware store to address 0, so the delivery the child dies of must be
   * SIGSEGV and nothing else. Accepting SIGABRT here would launder an engine
   * abort (e.g. an assertion or a failed dispatch) into a pass, which is
   * precisely the signal-identity violation this case exists to catch. */
  status = 0;
  check(run_child(child_default_segv, NULL, &status) == 0,
        "forked the real-SIGSEGV child");
  check(WIFSIGNALED(status), "the DEFAULT SIGSEGV child died of a signal");
  if (WIFSIGNALED(status))
    check(WTERMSIG(status) == SIGSEGV,
          "a real DEFAULT SIGSEGV fault terminates the child with SIGSEGV "
          "(any other terminating signal means the engine did not preserve the "
          "fault's signal identity)");
  check_u64(test_simple0(sym_alive), 0x5a17,
            "the parent survived the child's real SIGSEGV");
}

/* ------------------------------------------------------------------ *
 * main
 * ------------------------------------------------------------------ */

int main(int argc, char** argv) {
  int want_app;

  if (argc < 3) {
    printf("usage: %s <guest.so> <case>\n", argv[0]);
    printf("cases: alive host-preinit guest-replace native-override unity\n"
           "       not-init roundtrip invalid uc-mask longjmp flags-mask altstack\n"
           "       restart resethand resethand-race smc pagefix native-fault\n"
           "       native-c-fault chld concurrent fatal\n");
    return 2;
  }
  g_case = argv[2];
  box64_diag_set(0);

  /* The not-init case must run BEFORE the engine is brought up, and it exits
   * from here: it asserts ENOSYS from every entry point. */
  if (is_case("not-init")) {
    case_not_init();
    exit_result();
  }

  /* The native alt stack the APP installs before init. */
  g_altstack_len = 64 * 1024;
  g_altstack = mmap(NULL, g_altstack_len, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (g_altstack == MAP_FAILED) g_altstack = NULL;

  want_app = is_case("host-preinit") || is_case("altstack") || is_case("unity") ||
             is_case("roundtrip") || is_case("native-fault") ||
             is_case("native-c-fault");

  /* The SMC case wants the app's SIGSEGV action to be RESETHAND-capable, so it
   * installs that shape instead of the plain watchdog.
   *
   * The `fatal` case must NOT install the watchdog: it asserts that a REAL
   * DEFAULT SIGSEGV fault terminates the child with the fault's own signal
   * identity, and that requires SIGSEGV's captured pre-init action to be the
   * process's real default. With the watchdog installed the logical action is
   * NATIVE, so the child's store-to-NULL is (correctly) delivered to that native
   * handler, which returns, and the faulting instruction re-executes forever:
   * the child never terminates, and the case can never observe the DEFAULT
   * behaviour it declares. Every other case keeps the watchdog. */
  if (is_case("smc"))
    install_app_segv_resethand();
  else if (!is_case("fatal"))
    install_host_segv_watchdog();

  if (engine_up(want_app, argv[1]) != 0) {
    printf("[signal-actions] RESULT: FAIL (engine init)\n");
    return 1;
  }

  if (is_case("alive")) {
    check_u64(test_simple0(sym_alive), 0x5a17, "guest entry works");
    check(box64_signal_policy_get() == BOX64_SIGNAL_POLICY_FAST ||
              box64_signal_policy_get() == BOX64_SIGNAL_POLICY_COMPAT,
          "a signal policy is published");
  } else if (is_case("host-preinit")) {
    case_host_preinit();
  } else if (is_case("guest-replace")) {
    case_guest_replace();
  } else if (is_case("native-override")) {
    case_native_override();
  } else if (is_case("unity")) {
    case_unity();
  } else if (is_case("roundtrip")) {
    case_roundtrip();
  } else if (is_case("invalid")) {
    case_invalid();
  } else if (is_case("not-init")) {
    case_not_init();
  } else if (is_case("uc-mask")) {
    case_uc_mask();
  } else if (is_case("longjmp")) {
    case_longjmp();
  } else if (is_case("flags-mask")) {
    case_flags_mask();
  } else if (is_case("altstack")) {
    case_altstack();
  } else if (is_case("restart")) {
    case_restart();
  } else if (is_case("resethand")) {
    case_resethand();
  } else if (is_case("resethand-race")) {
    case_resethand_race();
  } else if (is_case("smc")) {
    case_smc();
  } else if (is_case("pagefix")) {
    case_pagefix();
  } else if (is_case("native-fault")) {
    case_native_fault();
  } else if (is_case("native-c-fault")) {
    case_native_c_fault();
  } else if (is_case("chld")) {
    case_chld();
  } else if (is_case("concurrent")) {
    case_concurrent();
  } else if (is_case("fatal")) {
    case_fatal();
  } else {
    printf("[signal-actions] unknown case '%s'\n", g_case);
    return 2;
  }

  /* The host SIGSEGV watchdog must never have fired UNINTENDED: a guest or JIT
   * fault reaching the host action is the wrong route. Two cases are excluded
   * because the application itself makes the host action run there, and each one
   * asserts that per round so the exclusion is not a hole:
   *   - `fatal` re-raises through the terminal default;
   *   - `native-c-fault`'s chaining round calls the app's saved NATIVE action on
   *     purpose (its plain round asserts zero, its chaining round asserts the
   *     count equals the number of chain calls). */
  if (!is_case("fatal") && !is_case("native-c-fault"))
    check(g_host_segv_hits == 0,
          "the host SIGSEGV action never fired (no wrong-route fault)");

  box64_lib_fini();
  exit_result();
  return 0;
}
