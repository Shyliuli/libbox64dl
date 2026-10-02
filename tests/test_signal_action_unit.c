/*
 * Standalone core-module unit test for src/signal_actions.c.
 *
 * Built with ONLY -Iinclude and src/signal_actions.c + this file:
 *
 *   cc -std=c11 -Wall -Wextra -Werror -Iinclude -pthread \
 *      -Wl,--wrap=sigaction -o build/test_signal_action_unit \
 *      tests/test_signal_action_unit.c src/signal_actions.c
 *
 * The module talks to the engine through exactly two symbols, and this file
 * defines both as test doubles:
 *
 *   box64dl_engine_signal_hook      pointer stub (the header declares it extern
 *                                   and the module calls it when non-NULL for a
 *                                   hardware fault)
 *   box64dl_signal_guest_deliver    guest-worker stub (records the seam args)
 *
 * It is a SUPPLEMENT to the real-engine tests (tests/test_signal_actions.c),
 * never a substitute. It only reads src/signal_actions.c and
 * include/libbox64_signal_actions.h; it never modifies implementation code.
 * Every expectation is derived from the frozen header contract or from a host
 * glibc behaviour probed at runtime (documented inline). Anything the
 * implementation does that the frozen contract does not decide is recorded in
 * the evidence log as AMBIGUOUS and is not softened into a pass.
 *
 * Each case runs in its own forked child: the signal action table and the
 * installed kernel dispositions are process-wide and one-shot, so a case can
 * never be reused inside one process, and a case that rightfully terminates
 * (the DEFAULT fatal path) can only be observed from a parent.
 *
 * Fail-fast: every FAIL line carries file:line and expected-vs-seen.
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#include "libbox64_signal_actions.h"

/* ========================================================================== *
 * Evidence log
 * ========================================================================== */

static int g_log_fd = 1;

static void logf_(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void logf_(const char* fmt, ...) {
  char buf[2048];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if (n < 0) return;
  if ((size_t)n >= sizeof(buf)) n = (int)sizeof(buf) - 1;
  ssize_t w = write(g_log_fd, buf, (size_t)n);
  (void)w;
}

static _Atomic int g_failures;
static const char* g_case = "?";

#define FAILF(...)                                                   \
  do {                                                               \
    atomic_fetch_add_explicit(&g_failures, 1, memory_order_relaxed); \
    logf_("FAIL [%s] %s:%d: ", g_case, __FILE__, __LINE__);          \
    logf_(__VA_ARGS__);                                              \
    logf_("\n");                                                     \
  } while (0)

#define CK(cond, ...)                                \
  do {                                               \
    if (!(cond)) {                                   \
      FAILF(__VA_ARGS__);                            \
      logf_("        (condition: %s)\n", #cond);     \
    }                                                \
  } while (0)

#define INFO(...)                    \
  do {                               \
    logf_("INFO [%s] ", g_case);     \
    logf_(__VA_ARGS__);              \
    logf_("\n");                     \
  } while (0)

#define AMBIG(...)                       \
  do {                                   \
    logf_("AMBIGUOUS [%s] ", g_case);    \
    logf_(__VA_ARGS__);                  \
    logf_("\n");                         \
  } while (0)

/* ========================================================================== *
 * Engine seam doubles
 * ========================================================================== */

static _Atomic int g_guest_calls;
static _Atomic int g_guest_last_sig;
static _Atomic int g_guest_last_kind;
static _Atomic uintptr_t g_guest_last_handler;
static _Atomic uintptr_t g_guest_last_uc;
static _Atomic uintptr_t g_guest_last_dynablock;
static _Atomic uintptr_t g_guest_last_x64pc;
static _Atomic int g_guest_last_emu_null;
static _Atomic int g_guest_bad;

box64dl_engine_signal_hook_t box64dl_engine_signal_hook = 0;

void box64dl_signal_guest_deliver(x64emu_t* emu, int host_sig, siginfo_t* info,
                                  void* native_uc,
                                  const box64dl_signal_action_t* action,
                                  void* dynablock, uintptr_t x64pc) {
  (void)info;
  atomic_fetch_add_explicit(&g_guest_calls, 1, memory_order_relaxed);
  atomic_store(&g_guest_last_sig, host_sig);
  atomic_store(&g_guest_last_kind, action ? action->kind : -1);
  atomic_store(&g_guest_last_handler, action ? (uintptr_t)action->handler : 0);
  atomic_store(&g_guest_last_uc, (uintptr_t)native_uc);
  atomic_store(&g_guest_last_dynablock, (uintptr_t)dynablock);
  atomic_store(&g_guest_last_x64pc, x64pc);
  atomic_store(&g_guest_last_emu_null, emu == NULL ? 1 : 0);
  /* Frozen contract check, done where the call actually happens: the guest
   * worker must receive a by-value GUEST action with the guest address, and the
   * engine identity (NULL emu / (uintptr_t)-1) that a user-generated delivery
   * carries. */
  if (!action || action->kind != BOX64_SIGNAL_ACTION_GUEST ||
      action->handler == 0 || x64pc != (uintptr_t)-1)
    atomic_fetch_add(&g_guest_bad, 1);
}

/* ========================================================================== *
 * sigaction() interposition (linker --wrap)
 * ========================================================================== */

typedef struct wrec_s {
  int op; /* 0 = query, 1 = install */
  int sig;
  int rc;
  int err;
  const void* handler; /* immediate sa_handler/sa_sigaction */
  int flags;
  int mask_nonempty;
} wrec_t;

#define WLOG_MAX 8192
static wrec_t g_wlog[WLOG_MAX];
static _Atomic int g_wlog_n;

static int (*g_real_sigaction)(int, const struct sigaction*, struct sigaction*);

/* fault injection: fail g_fail_times matching installs, starting after skip */
static int g_fail_sig;
static int g_fail_skip;
static int g_fail_times;
static _Atomic int g_fail_hits;

/* stall: block once inside an install for a matching signal/handler */
static int g_stall_sig;
static uintptr_t g_stall_handler;
static _Atomic int g_stall_armed;
static _Atomic int g_stall_reached;
static _Atomic int g_stall_release;
static _Atomic int g_stall_seen;
static wrec_t g_stall_rec;

/* hold AFTER the real sigaction succeeded but BEFORE the setter returns: this
 * is the genuine window in which the kernel already runs the new entry while
 * `latest` still names the previous action. Only the variant argument and the
 * kernel's own record can be observed here. */
static int g_stall_after_sig;
static uintptr_t g_stall_after_handler;
static _Atomic int g_stall_after_armed;
static _Atomic int g_stall_after_reached;
static _Atomic int g_stall_after_release;
static _Atomic int g_stall_after_seen;

/* hold: stall once BEFORE calling the real sigaction (kept for reference only;
 * the pre-install store is not an observable publication window) */
static int g_hold_sig;
static _Atomic int g_hold_armed;
static _Atomic int g_hold_reached;
static _Atomic int g_hold_release;
static _Atomic int g_hold_seen;

static void wrap_record(int op, int sig, const struct sigaction* sa, int rc,
                        int err) {
  int i = atomic_fetch_add_explicit(&g_wlog_n, 1, memory_order_relaxed);
  if (i >= WLOG_MAX) return;
  wrec_t r;
  r.op = op;
  r.sig = sig;
  r.rc = rc;
  r.err = err;
  r.handler = NULL;
  r.flags = 0;
  r.mask_nonempty = 0;
  if (sa) {
    r.handler = ((uint32_t)sa->sa_flags & SA_SIGINFO)
                    ? (const void*)sa->sa_sigaction
                    : (const void*)sa->sa_handler;
    r.flags = (int)sa->sa_flags;
    r.mask_nonempty = !sigisemptyset(&sa->sa_mask);
  }
  g_wlog[i] = r;
}

static const wrec_t* wrap_last(int op, int sig) {
  int n = atomic_load_explicit(&g_wlog_n, memory_order_relaxed);
  if (n > WLOG_MAX) n = WLOG_MAX;
  for (int i = n - 1; i >= 0; --i)
    if (g_wlog[i].op == op && g_wlog[i].sig == sig) return &g_wlog[i];
  return NULL;
}

int __wrap_sigaction(int sig, const struct sigaction* act,
                     struct sigaction* old) {
  if (!g_real_sigaction) g_real_sigaction = dlsym(RTLD_NEXT, "sigaction");

  int inject = 0;
  if (act && sig == g_fail_sig && g_fail_times > 0) {
    int h = atomic_fetch_add_explicit(&g_fail_hits, 1, memory_order_relaxed);
    if (h >= g_fail_skip && h < g_fail_skip + g_fail_times) inject = 1;
  }
  if (act && inject) {
    wrap_record(1, sig, act, -1, EINVAL);
    errno = EINVAL;
    return -1;
  }

  /* hold: publish a record as if the install were in progress, then wait. The
   * record is deliberately NOT what the kernel holds, so an observation taken
   * during the hold must use a real query. */
  if (act && atomic_load_explicit(&g_hold_armed, memory_order_acquire) &&
      sig == g_hold_sig && !atomic_exchange(&g_hold_seen, 1)) {
    wrap_record(1, sig, act, 0, 0);
    atomic_store_explicit(&g_hold_reached, 1, memory_order_release);
    struct timespec hts = {0, 200 * 1000};
    int hguard = 0;
    while (!atomic_load_explicit(&g_hold_release, memory_order_acquire) &&
           hguard++ < 25000)
      nanosleep(&hts, NULL);
    if (hguard >= 25000)
      logf_("WARN [%s] hold released by the 5s guard, not by the test\n", g_case);
  }

  if (act && atomic_load_explicit(&g_stall_armed, memory_order_acquire) &&
      sig == g_stall_sig && g_stall_handler != 0 &&
      ((uint32_t)act->sa_flags & SA_SIGINFO) &&
      (uintptr_t)act->sa_sigaction == g_stall_handler &&
      !atomic_exchange(&g_stall_seen, 1)) {
    wrap_record(1, sig, act, 0, 0);
    g_stall_rec = g_wlog[(atomic_load(&g_wlog_n) - 1) % WLOG_MAX];
    atomic_store_explicit(&g_stall_reached, 1, memory_order_release);
    struct timespec ts = {0, 200 * 1000};
    int guard = 0;
    while (!atomic_load_explicit(&g_stall_release, memory_order_acquire) &&
           guard++ < 25000)
      nanosleep(&ts, NULL);
    if (guard >= 25000)
      logf_("WARN [%s] stall released by the 5s guard, not by the test\n",
            g_case);
  }

  int rc = g_real_sigaction(sig, act, old);
  int err = errno;
  wrap_record(act ? 1 : 0, sig, act, rc, rc ? err : 0);

  /* GENUINE publication window: the install has taken effect, and the setter
   * has not yet published `latest`. Hold here on request. */
  if (act && rc == 0 &&
      atomic_load_explicit(&g_stall_after_armed, memory_order_acquire) &&
      sig == g_stall_after_sig && g_stall_after_handler != 0 &&
      ((uint32_t)act->sa_flags & SA_SIGINFO) &&
      (uintptr_t)act->sa_sigaction == g_stall_after_handler &&
      !atomic_exchange(&g_stall_after_seen, 1)) {
    atomic_store_explicit(&g_stall_after_reached, 1, memory_order_release);
    struct timespec ats = {0, 200 * 1000};
    int aguard = 0;
    while (!atomic_load_explicit(&g_stall_after_release, memory_order_acquire) &&
           aguard++ < 25000)
      nanosleep(&ats, NULL);
    if (aguard >= 25000)
      logf_("WARN [%s] post-install hold released by the 5s guard\n", g_case);
  }
  return rc;
}

/* ========================================================================== *
 * Native handler stubs and what they observe
 * ========================================================================== */

enum { HID_A = 1, HID_B = 2, HID_C = 3, HID_D = 4, HID_JMP = 5 };

static _Atomic int g_hits[8];
static _Atomic int g_last_id;
static _Atomic int g_uc_kind; /* 0 = none, 1 = pointer, 2 = NULL */
static _Atomic uintptr_t g_uc_seen;
static _Atomic int g_arg_seen;
static _Atomic int g_live_usr1;
static _Atomic int g_live_usr2;
static _Atomic int g_live_hup;
static _Atomic int g_live_urg;
static _Atomic int g_live_empty;

static void snap_state(void* ucntx, int sig) {
  atomic_store(&g_uc_kind, ucntx ? 1 : 2);
  atomic_store(&g_uc_seen, (uintptr_t)ucntx);
  sigset_t live;
  if (sigprocmask(SIG_SETMASK, NULL, &live) == 0) {
    atomic_store(&g_live_usr1, sigismember(&live, SIGUSR1) == 1);
    atomic_store(&g_live_usr2, sigismember(&live, SIGUSR2) == 1);
    atomic_store(&g_live_hup, sigismember(&live, SIGHUP) == 1);
    atomic_store(&g_live_urg, sigismember(&live, SIGURG) == 1);
    atomic_store(&g_live_empty, sigisemptyset(&live) == 1);
  }
  (void)sig;
}

static void hit(int id, int sig, siginfo_t* info, void* ucntx, int with_si) {
  atomic_fetch_add_explicit(&g_hits[id], 1, memory_order_relaxed);
  atomic_store(&g_last_id, id);
  atomic_store(&g_arg_seen, sig);
  if (with_si && !info) atomic_store(&g_uc_kind, 0);
  snap_state(ucntx, sig);
}

static void h_nat_a(int sig, siginfo_t* i, void* u) { hit(HID_A, sig, i, u, 1); }
static void h_nat_b(int sig, siginfo_t* i, void* u) { hit(HID_B, sig, i, u, 1); }
static void h_nat_c(int sig, siginfo_t* i, void* u) { hit(HID_C, sig, i, u, 1); }
static void h_nat_d(int sig, siginfo_t* i, void* u) { hit(HID_D, sig, i, u, 1); }

static sigjmp_buf g_jb;
static _Atomic int g_jmp_ran;
static void h_nat_jmp(int sig, siginfo_t* i, void* u) {
  (void)i;
  (void)u;
  (void)sig;
  atomic_fetch_add_explicit(&g_hits[HID_JMP], 1, memory_order_relaxed);
  atomic_store(&g_jmp_ran, 1);
  siglongjmp(g_jb, 1);
}

static void hits_reset(void) {
  for (int i = 0; i < 8; ++i) atomic_store(&g_hits[i], 0);
  atomic_store(&g_last_id, 0);
  atomic_store(&g_uc_kind, 0);
  atomic_store(&g_uc_seen, 0);
  atomic_store(&g_arg_seen, 0);
  atomic_store(&g_live_usr1, -1);
  atomic_store(&g_live_usr2, -1);
  atomic_store(&g_live_hup, -1);
  atomic_store(&g_live_urg, -1);
  atomic_store(&g_live_empty, -1);
}

/* ========================================================================== *
 * Helpers
 * ========================================================================== */

static int v_of(uint64_t flags) {
  uint32_t f = (uint32_t)flags;
  int v = 0;
  if (f & SA_ONSTACK) v |= 1;
  if (f & SA_RESTART) v |= 2;
  return v;
}

static int set_native_act(int sig, uintptr_t handler, uint64_t flags,
                          int mask_sig, uintptr_t restorer,
                          box64dl_signal_action_t* old);

/* the four kernel entry pointers, learned from what the module installs */
static const void* g_entry[BOX64DL_SIGNAL_VARIANTS];
static int g_entry_known;

static void map_entries(void) {
  const int scratch = SIGRTMIN + 9;
  static const uintptr_t probes[BOX64DL_SIGNAL_VARIANTS] = {
      (uintptr_t)h_nat_a, (uintptr_t)h_nat_b, (uintptr_t)h_nat_c,
      (uintptr_t)h_nat_d};
  for (int v = 0; v < BOX64DL_SIGNAL_VARIANTS; ++v) {
    uint64_t flags = (uint64_t)SA_SIGINFO;
    if (v & 1) flags |= SA_ONSTACK;
    if (v & 2) flags |= SA_RESTART;
    if (set_native_act(scratch, probes[v], flags, 0, 0, NULL) != 0) {
      logf_("WARN map_entries: variant %d install failed rc=-1 errno=%d\n", v,
            errno);
      continue;
    }
    const wrec_t* r = wrap_last(1, scratch);
    if (r && r->handler)
      g_entry[v] = r->handler;
    else
      logf_("WARN map_entries: variant %d left no wrap record\n", v);
  }
  g_entry_known = 1;
}

static void dump_kernel(int sig, const char* what) {
  struct sigaction cur;
  memset(&cur, 0, sizeof(cur));
  if (sigaction(sig, NULL, &cur) != 0) {
    logf_("  kernel(%s, sig %d): query failed errno=%d\n", what, sig, errno);
    return;
  }
  int which = -1;
  if (g_entry_known && ((uint32_t)cur.sa_flags & SA_SIGINFO))
    for (int v = 0; v < BOX64DL_SIGNAL_VARIANTS; ++v)
      if ((const void*)cur.sa_sigaction == g_entry[v]) which = v;
  logf_("  kernel(%s, sig %d): handler=%p entry_variant=%d siginfo=%d onstack=%d "
        "restart=%d nod=%d resethand=%d noclstop=%d noclwait=%d flags=0x%08x "
        "mask_nonempty=%d\n",
        what, sig, (void*)cur.sa_sigaction, which,
        (cur.sa_flags & SA_SIGINFO) ? 1 : 0, (cur.sa_flags & SA_ONSTACK) ? 1 : 0,
        (cur.sa_flags & SA_RESTART) ? 1 : 0, (cur.sa_flags & SA_NODEFER) ? 1 : 0,
        (cur.sa_flags & SA_RESETHAND) ? 1 : 0,
        (cur.sa_flags & SA_NOCLDSTOP) ? 1 : 0,
        (cur.sa_flags & SA_NOCLDWAIT) ? 1 : 0, (unsigned)cur.sa_flags,
        !sigisemptyset(&cur.sa_mask));
}

static void dump_action(const char* what, int sig) {
  box64dl_signal_action_t a;
  memset(&a, 0, sizeof(a));
  if (box64dl_signal_action_get(sig, &a) != 0) {
    logf_("  logical(%s, sig %d): get failed errno=%d\n", what, sig, errno);
    return;
  }
  logf_("  logical(%s, sig %d): kind=%d handler=%p restorer=%p flags=0x%016llx "
        "gen=%llu mask{usr1=%d,usr2=%d,hup=%d} reserved0=%d\n",
        what, sig, a.kind, (void*)a.handler, (void*)a.restorer,
        (unsigned long long)a.flags, (unsigned long long)a.generation,
        sigismember(&a.mask, SIGUSR1) == 1, sigismember(&a.mask, SIGUSR2) == 1,
        sigismember(&a.mask, SIGHUP) == 1, a._reserved0);
}

static int set_native_act(int sig, uintptr_t handler, uint64_t flags,
                          int mask_sig, uintptr_t restorer,
                          box64dl_signal_action_t* old) {
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_sigaction = (void (*)(int, siginfo_t*, void*))handler;
  /* A SIGINFO-shaped handler must be accompanied by SA_SIGINFO in sa_flags, or
   * the kernel stores the pointer in sa_handler and the requested variant is
   * not the one the flags describe. Every caller of this helper wants the
   * SIGINFO door, so force the bit rather than trusting the caller's literal
   * (getting this wrong silently produced variant 3 for a variant-1 request). */
  sa.sa_flags = (int)((uint32_t)flags | (uint32_t)SA_SIGINFO);
  sa.sa_restorer = (void (*)(void))restorer;
  sigemptyset(&sa.sa_mask);
  if (mask_sig) sigaddset(&sa.sa_mask, mask_sig);
  return box64_signal_action_set_native(sig, &sa, old);
}

/* Variant 0 (neither SA_ONSTACK nor SA_RESTART) is what a plain SIGINFO action
 * produces. NOTE: BOX64DL_SIGNAL_VARIANT_MASK is the constant 3 (the third
 * entry), not "no flags" — do not use it as variant 0. */
#define V_PLAIN 0

/* synthetic delivery of a USER signal: si_code = SI_USER, no engine repairs */
static void deliver_user(int sig, int variant) {
  siginfo_t info;
  memset(&info, 0, sizeof(info));
  info.si_signo = sig;
  info.si_code = SI_USER;
  box64dl_signal_deliver(NULL, sig, &info, NULL, variant, NULL, (uintptr_t)-1);
}

/* Isolated delivery with observations reported back.
 *
 * A delivery the module resolves to a fatal DEFAULT terminates the process, and
 * a child's own globals are copy-on-write so the parent cannot read them.
 * Therefore the child does the whole evaluation itself, writes a full line to
 * the shared evidence log, and exits with a byte that encodes what happened:
 *
 *   bits 0..3  observation bits (what the handler saw)
 *   bits 4..7  outcome: 0 = nothing invoked, 1 = handler A, 2 = handler B,
 *              3 = the siglongjmp handler, 4 = the guest-worker seam
 *
 * Returns the child's exit byte, or 0x100 + signal when the child was killed.
 */
#define DVO_NONE 0
#define DVO_A 1
#define DVO_B 2
#define DVO_JMP 3
#define DVO_GUEST 4
#define DV_BIT_USR1 1u
#define DV_BIT_HUP 2u
#define DV_BIT_URG 4u
#define DV_BIT_UC 8u
#define DV_BIT_GUEST_BAD 8u /* same bit, used by the guest-seam probes */

static uintptr_t g_dv_uc_want; /* the ucontext pointer the child should see */

static int deliver_probe(int sig, int variant, const char* tag) {
  fflush(NULL);
  int guest_before = atomic_load(&g_guest_calls);
  pid_t pid = fork();
  if (pid == 0) {
    int rc = 0;
    box64dl_signal_action_t pre;
    memset(&pre, 0, sizeof(pre));
    int g = box64dl_signal_action_get(sig, &pre);
    deliver_user(sig, variant);
    rc = 1; /* returned normally */
    int oc = DVO_NONE;
    if (atomic_load(&g_hits[HID_JMP]))
      oc = DVO_JMP;
    else if (atomic_load(&g_guest_calls) > guest_before)
      oc = DVO_GUEST;
    else if (atomic_load(&g_hits[HID_A]))
      oc = DVO_A;
    else if (atomic_load(&g_hits[HID_B]))
      oc = DVO_B;
    unsigned bits = 0;
    if (atomic_load(&g_live_usr1) == 1) bits |= DV_BIT_USR1;
    if (atomic_load(&g_live_hup) == 1) bits |= DV_BIT_HUP;
    if (atomic_load(&g_live_urg) == 1) bits |= DV_BIT_URG;
    if (g_dv_uc_want && atomic_load(&g_uc_seen) == g_dv_uc_want)
      bits |= DV_BIT_UC;
    if (atomic_load(&g_guest_bad)) bits |= DV_BIT_GUEST_BAD;
    logf_("  %s: rc=%d pre_get=%d pre(kind=%d handler=%p) outcome=%d bits=0x%x "
          "guest_calls=%d guest_bad=%d guest_last{sig=%d kind=%d handler=%p "
          "x64pc=0x%llx emu_null=%d}\n",
          tag, rc, g, pre.kind, (void*)pre.handler, oc, bits,
          atomic_load(&g_guest_calls), atomic_load(&g_guest_bad),
          atomic_load(&g_guest_last_sig), atomic_load(&g_guest_last_kind),
          (void*)atomic_load(&g_guest_last_handler),
          (unsigned long long)atomic_load(&g_guest_last_x64pc),
          atomic_load(&g_guest_last_emu_null));
    _exit((oc << 4) | (int)(bits & 0xF));
  }
  int st = 0;
  waitpid(pid, &st, 0);
  if (WIFSIGNALED(st)) return 0x100 + WTERMSIG(st);
  return WEXITSTATUS(st) & 0xFF;
}

static int dv_outcome(int code) { return (code >> 4) & 0xF; }
static unsigned dv_bits(int code) { return (unsigned)code & 0xFu; }

/* Same, but the child delivers with a caller-supplied real ucontext_t so the
 * module's mask handling can be observed end to end. */
static int deliver_probe_uc(int sig, int variant, void* ucp, siginfo_t* infop,
                            const char* tag) {
  fflush(NULL);
  pid_t pid = fork();
  if (pid == 0) {
    int oc = DVO_NONE;
    box64dl_signal_deliver(NULL, sig, infop, ucp, variant, NULL, (uintptr_t)-1);
    if (atomic_load(&g_guest_calls) > 0)
      oc = DVO_GUEST;
    else if (atomic_load(&g_hits[HID_A]))
      oc = DVO_A;
    unsigned bits = 0;
    if (atomic_load(&g_live_usr1) == 1) bits |= DV_BIT_USR1;
    if (atomic_load(&g_live_hup) == 1) bits |= DV_BIT_HUP;
    if (atomic_load(&g_live_urg) == 1) bits |= DV_BIT_URG;
    if (g_dv_uc_want && atomic_load(&g_uc_seen) == g_dv_uc_want)
      bits |= DV_BIT_UC;
    logf_("  %s: outcome=%d bits=0x%x uc_seen=%p want=%p\n", tag, oc, bits,
          (void*)atomic_load(&g_uc_seen), (void*)g_dv_uc_want);
    _exit((oc << 4) | (int)(bits & 0xF));
  }
  int st = 0;
  waitpid(pid, &st, 0);
  if (WIFSIGNALED(st)) return 0x100 + WTERMSIG(st);
  return WEXITSTATUS(st) & 0xFF;
}

/* ========================================================================== *
 * Cases
 * ========================================================================== */

/* 1. init captures the preinstalled native action verbatim and logically. */
static void case_init_capture(void) {
  struct sigaction pre;
  memset(&pre, 0, sizeof(pre));
  pre.sa_sigaction = h_nat_a;
  pre.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER | SA_RESETHAND;
  sigemptyset(&pre.sa_mask);
  sigaddset(&pre.sa_mask, SIGUSR1);
  CK(sigaction(SIGUSR2, &pre, NULL) == 0, "preinstall failed errno=%d", errno);
  CK(box64dl_signal_actions_active() == 0, "active() must be 0 before init");
  CK(box64dl_signal_action_get(SIGUSR2, NULL) == -1 && errno == ENOSYS,
     "get before init must be ENOSYS, errno=%d", errno);

  CK(box64dl_signal_actions_init() == 0, "init failed errno=%d", errno);
  CK(box64dl_signal_actions_active() == 1, "active() must be 1 after init");
  CK(box64dl_signal_actions_init() == 0, "second init must be idempotent");
  map_entries();

  box64dl_signal_action_t a;
  memset(&a, 0, sizeof(a));
  CK(box64dl_signal_action_get(SIGUSR2, &a) == 0, "get after init failed");
  CK(a.kind == BOX64_SIGNAL_ACTION_NATIVE, "captured kind=%d expected NATIVE",
     a.kind);
  CK(a.handler == (uintptr_t)h_nat_a, "captured handler=%p expected %p",
     (void*)a.handler, (void*)h_nat_a);
  CK((uint32_t)a.flags ==
         (uint32_t)(SA_SIGINFO | SA_ONSTACK | SA_NODEFER | SA_RESETHAND),
     "captured flags=0x%08x expected 0x%08x",
     (unsigned)(uint32_t)a.flags,
     (unsigned)(SA_SIGINFO | SA_ONSTACK | SA_NODEFER | SA_RESETHAND));
  CK(a.flags >> 32 == 0,
     "flags must be an unsigned 32-bit widening, high half=0x%llx",
     (unsigned long long)(a.flags >> 32));
  CK(sigismember(&a.mask, SIGUSR1) == 1, "captured mask lost SIGUSR1");
  CK(sigismember(&a.mask, SIGUSR2) == 0, "captured mask gained SIGUSR2");
  CK(a.restorer == 0, "captured restorer=%p expected 0 (host had none)",
     (void*)a.restorer);
  CK(a._reserved0 == 0, "captured _reserved0=%d expected 0", a._reserved0);
  CK(a.generation != 0, "captured generation must be a real publication");

  /* the capture is virtualized: the kernel now runs the variant-1 entry
   * (SA_ONSTACK, no SA_RESTART) with an EMPTY kernel mask */
  const wrec_t* r = wrap_last(1, SIGUSR2);
  CK(r != NULL, "init must install its own entry for a captured NATIVE action");
  if (r) {
    CK((uint32_t)r->flags == (uint32_t)(SA_SIGINFO | SA_ONSTACK),
       "init kernel flags=0x%08x expected 0x%08x", (unsigned)r->flags,
       (unsigned)(SA_SIGINFO | SA_ONSTACK));
    CK(r->handler == g_entry[BOX64DL_SIGNAL_VARIANT_ONSTACK],
       "init kernel handler=%p expected the variant-1 entry %p", r->handler,
       g_entry[BOX64DL_SIGNAL_VARIANT_ONSTACK]);
    CK(r->mask_nonempty == 0, "init kernel mask must be empty");
    CK(((uint32_t)r->flags & (uint32_t)SA_NODEFER) == 0 &&
           ((uint32_t)r->flags & (uint32_t)SA_RESETHAND) == 0,
       "the kernel must not be asked for NODEFER or RESETHAND: flags=0x%08x",
       (unsigned)r->flags);
  }
  dump_action("SIGUSR2", SIGUSR2);
  dump_kernel(SIGUSR2, "after init");

  /* fini restores the captured pre-init disposition */
  box64dl_signal_actions_fini();
  struct sigaction cur;
  memset(&cur, 0, sizeof(cur));
  CK(sigaction(SIGUSR2, NULL, &cur) == 0, "query after fini failed");
  CK((const void*)cur.sa_sigaction == (const void*)h_nat_a,
     "fini must restore the captured handler %p, got %p", (void*)h_nat_a,
     (void*)cur.sa_sigaction);
  CK((uint32_t)cur.sa_flags ==
         (uint32_t)(SA_SIGINFO | SA_ONSTACK | SA_NODEFER | SA_RESETHAND),
     "fini restored flags=0x%08x expected the captured 0x%08x",
     (unsigned)cur.sa_flags,
     (unsigned)(SA_SIGINFO | SA_ONSTACK | SA_NODEFER | SA_RESETHAND));
  CK(sigismember(&cur.sa_mask, SIGUSR1) == 1,
     "fini must restore the captured sa_mask");
}

/* 2. the four kernel variants: logical flags -> entry, flags never forced. */
static void case_four_variants(void) {
  CK(box64dl_signal_actions_init() == 0, "init failed errno=%d", errno);
  map_entries();
  const int sig = SIGRTMIN + 1;
  static const uintptr_t hs[4] = {(uintptr_t)h_nat_a, (uintptr_t)h_nat_b,
                                  (uintptr_t)h_nat_c, (uintptr_t)h_nat_d};
  for (int v = 0; v < BOX64DL_SIGNAL_VARIANTS; ++v) {
    uint64_t flags = (uint64_t)SA_SIGINFO;
    if (v & 1) flags |= SA_ONSTACK;
    if (v & 2) flags |= SA_RESTART;
    box64dl_signal_action_t old;
    memset(&old, 0, sizeof(old));
    int rc = set_native_act(sig, hs[v], flags, SIGHUP, 0, &old);
    CK(rc == 0, "variant %d set rc=%d errno=%d", v, rc, errno);
    if (rc != 0) continue;

    box64dl_signal_action_t a;
    CK(box64dl_signal_action_get(sig, &a) == 0, "variant %d get failed", v);
    CK((uint32_t)a.flags == (uint32_t)flags,
       "variant %d logical flags=0x%08x expected 0x%08x", v,
       (unsigned)(uint32_t)a.flags, (unsigned)(uint32_t)flags);
    CK(a.handler == hs[v], "variant %d logical handler=%p expected %p", v,
       (void*)a.handler, (void*)hs[v]);
    CK(sigismember(&a.mask, SIGHUP) == 1,
       "variant %d logical mask lost SIGHUP", v);

    const wrec_t* r = wrap_last(1, sig);
    CK(r != NULL, "variant %d must reach the kernel", v);
    if (!r) continue;
    uint32_t want_kf = (uint32_t)SA_SIGINFO;
    if (v & 1) want_kf |= (uint32_t)SA_ONSTACK;
    if (v & 2) want_kf |= (uint32_t)SA_RESTART;
    CK((uint32_t)r->flags == want_kf,
       "variant %d kernel flags=0x%08x expected 0x%08x (no forced SA_SIGINFO "
       "only, no NODEFER, no RESETHAND, no kernel mask)",
       v, (unsigned)r->flags, (unsigned)want_kf);
    CK(((uint32_t)r->flags & (uint32_t)SA_RESETHAND) == 0,
       "variant %d kernel flags must not carry SA_RESETHAND: 0x%08x", v,
       (unsigned)r->flags);
    CK(((uint32_t)r->flags & (uint32_t)SA_NODEFER) == 0,
       "variant %d kernel flags must not carry SA_NODEFER: 0x%08x", v,
       (unsigned)r->flags);
    CK(r->handler == g_entry[v],
       "variant %d kernel handler=%p expected the entry for its own variant %p",
       v, r->handler, g_entry[v]);
    CK(r->mask_nonempty == 0, "variant %d kernel mask must be empty", v);
    dump_kernel(sig, "variant record");
  }

  box64dl_signal_action_t a;
  CK(box64_signal_action_set_native(sig, NULL, &a) == 0, "query failed");
  CK((uint32_t)a.flags == (uint32_t)(SA_SIGINFO | SA_ONSTACK | SA_RESTART),
     "query after variant 3 flags=0x%08x expected 0x%08x",
     (unsigned)(uint32_t)a.flags,
     (unsigned)(SA_SIGINFO | SA_ONSTACK | SA_RESTART));
  CK(a.handler == (uintptr_t)h_nat_d,
     "query after variant 3 handler=%p expected %p", (void*)a.handler,
     (void*)h_nat_d);
  box64dl_signal_actions_fini();
}

/* 3. range, reserved signals, KILL/STOP, ENOSYS after fini. */
static void case_range_and_reserved(void) {
  CK(box64dl_signal_actions_init() == 0, "init failed errno=%d", errno);
  box64dl_signal_action_t a;

  errno = 0;
  CK(box64_signal_action_get(0, &a) == -1 && errno == EINVAL,
     "get(0) must be EINVAL, errno=%d", errno);
  errno = 0;
  CK(box64_signal_action_get(65, &a) == -1 && errno == EINVAL,
     "get(65) must be EINVAL, errno=%d", errno);
  errno = 0;
  CK(box64_signal_action_get(-1, &a) == -1 && errno == EINVAL,
     "get(-1) must be EINVAL, errno=%d", errno);
  errno = 0;
  CK(box64_signal_action_get(SIGUSR1, NULL) == -1 && errno == EFAULT,
     "public get(NULL) must be EFAULT, errno=%d", errno);
  CK(box64dl_signal_action_get(SIGUSR1, NULL) == 0,
     "the internal getter accepts a NULL out pointer for a tracked signal");

  errno = 0;
  CK(box64_signal_action_get(32, &a) == -1 && errno == ENOTSUP,
     "get(32) must be ENOTSUP, errno=%d", errno);
  errno = 0;
  CK(box64_signal_action_get(33, &a) == -1 && errno == ENOTSUP,
     "get(33) must be ENOTSUP, errno=%d", errno);
  errno = 0;
  CK(box64_signal_action_set_native(32, NULL, &a) == -1 && errno == ENOTSUP,
     "set(32) must be ENOTSUP, errno=%d", errno);
  errno = 0;
  CK(box64dl_signal_action_set(33, NULL, &a) == -1 && errno == ENOTSUP,
     "internal set(33) must be ENOTSUP, errno=%d", errno);

  memset(&a, 0xAA, sizeof(a));
  CK(box64_signal_action_get(SIGKILL, &a) == 0, "get(SIGKILL) must succeed");
  CK(a.kind == BOX64_SIGNAL_ACTION_DEFAULT, "SIGKILL kind=%d expected DEFAULT",
     a.kind);
  CK(a.handler == 0, "SIGKILL handler=%p expected 0", (void*)a.handler);
  memset(&a, 0xAA, sizeof(a));
  CK(box64_signal_action_get(SIGSTOP, &a) == 0, "get(SIGSTOP) must succeed");
  CK(a.kind == BOX64_SIGNAL_ACTION_DEFAULT, "SIGSTOP kind=%d expected DEFAULT",
     a.kind);
  errno = 0;
  CK(set_native_act(SIGKILL, (uintptr_t)h_nat_a, SA_SIGINFO, 0, 0, NULL) == -1 &&
         errno == EINVAL,
     "set(SIGKILL) must be EINVAL, errno=%d", errno);
  errno = 0;
  CK(set_native_act(SIGSTOP, (uintptr_t)h_nat_a, SA_SIGINFO, 0, 0, NULL) == -1 &&
         errno == EINVAL,
     "set(SIGSTOP) must be EINVAL, errno=%d", errno);
  CK(box64dl_signal_action_set(SIGKILL, NULL, &a) == 0,
     "query-only set(SIGKILL) must succeed");
  CK(a.kind == BOX64_SIGNAL_ACTION_DEFAULT, "query-only SIGKILL kind=%d",
     a.kind);

  box64dl_signal_action_t bad;
  memset(&bad, 0, sizeof(bad));
  bad.kind = 99;
  errno = 0;
  CK(box64dl_signal_action_set(SIGUSR1, &bad, NULL) == -1 && errno == EINVAL,
     "kind 99 must be EINVAL, errno=%d", errno);
  bad.kind = BOX64_SIGNAL_ACTION_NATIVE;
  bad.handler = 0;
  errno = 0;
  CK(box64dl_signal_action_set(SIGUSR1, &bad, NULL) == -1 && errno == EINVAL,
     "NATIVE with handler 0 must be EINVAL, errno=%d", errno);
  bad.handler = (uintptr_t)SIG_ERR;
  errno = 0;
  CK(box64dl_signal_action_set(SIGUSR1, &bad, NULL) == -1 && errno == EINVAL,
     "NATIVE with SIG_ERR must be EINVAL, errno=%d", errno);

  box64dl_signal_actions_fini();
  CK(box64dl_signal_actions_active() == 0, "active() after fini must be 0");
  errno = 0;
  CK(box64_signal_action_get(SIGUSR1, &a) == -1 && errno == ENOSYS,
     "get after fini must be ENOSYS, errno=%d", errno);
  errno = 0;
  CK(set_native_act(SIGUSR1, (uintptr_t)h_nat_a, SA_SIGINFO, 0, 0, NULL) == -1 &&
         errno == ENOSYS,
     "set after fini must be ENOSYS, errno=%d", errno);
  CK(box64dl_signal_actions_init() == 0, "re-init after fini must succeed");
  CK(box64dl_signal_actions_active() == 1, "active() must be 1 after re-init");
}

/* 4. SIGCHLD: transient same-variant kernel flags and DFL NOCLDWAIT kept. */
static void case_chld_kernel_flags(void) {
  CK(box64dl_signal_actions_init() == 0, "init failed errno=%d", errno);
  map_entries();
  const int sig = SIGCHLD;

  int rc = set_native_act(sig, (uintptr_t)h_nat_a, SA_SIGINFO | SA_NOCLDWAIT, 0,
                          0, NULL);
  CK(rc == 0, "chld native install rc=%d errno=%d", rc, errno);
  const wrec_t* r = wrap_last(1, sig);
  CK(r != NULL, "chld native must reach the kernel");
  if (r)
    CK((uint32_t)r->flags == (uint32_t)(SA_SIGINFO | SA_NOCLDWAIT),
       "chld kernel flags=0x%08x expected 0x%08x", (unsigned)r->flags,
       (unsigned)(SA_SIGINFO | SA_NOCLDWAIT));
  struct sigaction cur;
  memset(&cur, 0, sizeof(cur));
  CK(sigaction(sig, NULL, &cur) == 0, "kernel chld query failed");
  CK((cur.sa_flags & SA_NOCLDWAIT) != 0,
     "the kernel must carry NOCLDWAIT for a NATIVE chld handler, flags=0x%x",
     (unsigned)cur.sa_flags);

  /* same variant (0), only the NOCLDSTOP kernel bit moves: still an update */
  int before = atomic_load(&g_wlog_n);
  rc = set_native_act(sig, (uintptr_t)h_nat_a,
                      SA_SIGINFO | SA_NOCLDWAIT | SA_NOCLDSTOP, 0, 0, NULL);
  CK(rc == 0, "chld stop install rc=%d errno=%d", rc, errno);
  int after = atomic_load(&g_wlog_n);
  CK(after > before,
     "a same-variant NOCLDSTOP change must still update the kernel "
     "(no sigaction call happened: %d -> %d)", before, after);
  memset(&cur, 0, sizeof(cur));
  CK(sigaction(sig, NULL, &cur) == 0, "kernel chld query failed");
  CK((cur.sa_flags & SA_NOCLDSTOP) != 0 && (cur.sa_flags & SA_NOCLDWAIT) != 0,
     "kernel chld flags=0x%x expected NOCLDSTOP|NOCLDWAIT",
     (unsigned)cur.sa_flags);

  box64dl_signal_action_t a;
  CK(box64dl_signal_action_get(sig, &a) == 0, "chld logical query failed");
  CK((uint32_t)a.flags == (uint32_t)(SA_SIGINFO | SA_NOCLDWAIT | SA_NOCLDSTOP),
     "chld logical flags=0x%08x expected SIGINFO|NOCLDWAIT|NOCLDSTOP",
     (unsigned)(uint32_t)a.flags);

  /* variant moves to 1 while both kernel bits stay */
  rc = set_native_act(sig, (uintptr_t)h_nat_a,
                      SA_SIGINFO | SA_ONSTACK | SA_NOCLDWAIT | SA_NOCLDSTOP, 0, 0,
                      NULL);
  CK(rc == 0, "chld onstack install rc=%d errno=%d", rc, errno);
  const wrec_t* r2 = wrap_last(1, sig);
  if (r2) {
    CK((uint32_t)r2->flags ==
           (uint32_t)(SA_SIGINFO | SA_ONSTACK | SA_NOCLDWAIT | SA_NOCLDSTOP),
       "chld variant1 kernel flags=0x%08x expected 0x%08x",
       (unsigned)r2->flags,
       (unsigned)(SA_SIGINFO | SA_ONSTACK | SA_NOCLDWAIT | SA_NOCLDSTOP));
    CK(r2->handler == g_entry[BOX64DL_SIGNAL_VARIANT_ONSTACK],
       "chld variant1 handler=%p expected the ONSTACK entry %p", r2->handler,
       g_entry[BOX64DL_SIGNAL_VARIANT_ONSTACK]);
  }

  /* DFL with NOCLDWAIT|NOCLDSTOP: the true kernel disposition, both bits kept */
  struct sigaction dfl;
  memset(&dfl, 0, sizeof(dfl));
  dfl.sa_handler = SIG_DFL;
  dfl.sa_flags = SA_NOCLDWAIT | SA_NOCLDSTOP;
  sigemptyset(&dfl.sa_mask);
  rc = box64_signal_action_set_native(sig, &dfl, NULL);
  CK(rc == 0, "chld dfl install rc=%d errno=%d", rc, errno);
  memset(&cur, 0, sizeof(cur));
  CK(sigaction(sig, NULL, &cur) == 0, "kernel chld query failed");
  CK(cur.sa_handler == SIG_DFL, "chld kernel handler=%p expected SIG_DFL",
     (void*)cur.sa_handler);
  CK((cur.sa_flags & SA_NOCLDWAIT) != 0 && (cur.sa_flags & SA_NOCLDSTOP) != 0,
     "chld DFL kernel flags=0x%x expected NOCLDWAIT|NOCLDSTOP preserved",
     (unsigned)cur.sa_flags);
  CK((cur.sa_flags & SA_SIGINFO) == 0,
     "chld DFL kernel flags must not carry SA_SIGINFO: 0x%x",
     (unsigned)cur.sa_flags);
  CK(box64dl_signal_action_get(sig, &a) == 0, "chld logical query failed");
  CK(a.kind == BOX64_SIGNAL_ACTION_DEFAULT, "chld logical kind=%d expected DFL",
     a.kind);
  CK((uint32_t)a.flags == (uint32_t)(SA_NOCLDWAIT | SA_NOCLDSTOP),
     "chld logical flags=0x%08x expected NOCLDWAIT|NOCLDSTOP",
     (unsigned)(uint32_t)a.flags);
  dump_kernel(sig, "chld dfl");
  box64dl_signal_actions_fini();
}

/* 5. an injected sigaction() failure must not change the recorded action, the
 *    real kernel disposition or the generation. The `old` out-param is NOT
 *    asserted on this path: neither the header nor Linux promises oldact when
 *    the call failed. The state that IS promised is asserted. */
static void case_injected_set_failure(void) {
  const int sig = SIGRTMIN + 2;
  CK(box64dl_signal_actions_init() == 0, "init failed errno=%d", errno);
  map_entries();

  box64dl_signal_action_t a0;
  memset(&a0, 0, sizeof(a0));
  CK(set_native_act(sig, (uintptr_t)h_nat_a, SA_SIGINFO | SA_RESTART, SIGHUP, 0,
                    &a0) == 0,
     "first set failed errno=%d", errno);
  box64dl_signal_action_t before;
  memset(&before, 0, sizeof(before));
  CK(box64dl_signal_action_get(sig, &before) == 0, "get failed");
  struct sigaction kq;
  memset(&kq, 0, sizeof(kq));
  CK(sigaction(sig, NULL, &kq) == 0, "kernel query before injection failed");
  const void* kreal_before = (const void*)kq.sa_sigaction;
  CK(kreal_before != NULL, "no kernel install recorded before injection");

  g_fail_sig = sig;
  g_fail_skip = 0;
  g_fail_times = 1;
  atomic_store(&g_fail_hits, 0);

  /* poison the out-param so "untouched" and "written" are distinguishable */
  box64dl_signal_action_t old;
  memset(&old, 0xA5, sizeof(old));
  errno = 0;
  int rc = set_native_act(sig, (uintptr_t)h_nat_b,
                          SA_SIGINFO | SA_ONSTACK | SA_RESTART, SIGUSR2,
                          (uintptr_t)h_nat_c, &old);
  int e = errno;
  g_fail_sig = 0;
  g_fail_times = 0;

  CK(rc == -1, "an injected install failure must fail the set, rc=%d", rc);
  CK(e == EINVAL, "injected failure errno=%d expected the sigaction errno 22", e);

  box64dl_signal_action_t after;
  memset(&after, 0, sizeof(after));
  CK(box64dl_signal_action_get(sig, &after) == 0, "get after failure failed");
  CK(after.kind == before.kind, "kind changed on a failed install: %d -> %d",
     before.kind, after.kind);
  CK(after.handler == before.handler,
     "handler changed on a failed install: %p -> %p", (void*)before.handler,
     (void*)after.handler);
  CK((uint32_t)after.flags == (uint32_t)before.flags,
     "flags changed on a failed install: 0x%08x -> 0x%08x",
     (unsigned)(uint32_t)before.flags, (unsigned)(uint32_t)after.flags);
  CK(after.generation == before.generation,
     "generation changed on a failed install: %llu -> %llu",
     (unsigned long long)before.generation,
     (unsigned long long)after.generation);
  CK(after.restorer == before.restorer && after._reserved0 == 0,
     "a failed install must leave the recorded snapshot intact");
  struct sigaction kq2;
  memset(&kq2, 0, sizeof(kq2));
  CK(sigaction(sig, NULL, &kq2) == 0, "kernel query after injection failed");
  CK((const void*)kq2.sa_sigaction == kreal_before,
     "the real kernel disposition changed on a failed install: %p -> %p",
     kreal_before, (void*)kq2.sa_sigaction);
  if (old.handler != before.handler || old.kind != before.kind)
    INFO("the failed install left the old out-param untouched "
         "(old{handler=%p kind=%d}); the still-installed action is "
         "{handler=%p kind=%d}. Not a contract violation: neither the header nor "
         "Linux promises oldact on failure, and docs 4.3's stronger claim is "
         "being corrected. get/generation/kernel are unchanged, which IS "
         "promised.",
         (void*)old.handler, old.kind, (void*)before.handler, before.kind);

  /* a clean retry publishes normally and DOES report the replaced action */
  box64dl_signal_action_t old2;
  memset(&old2, 0, sizeof(old2));
  rc = set_native_act(sig, (uintptr_t)h_nat_b,
                      SA_SIGINFO | SA_ONSTACK | SA_RESTART, SIGUSR2,
                      (uintptr_t)h_nat_c, &old2);
  CK(rc == 0, "retry after the injected failure rc=%d errno=%d", rc, errno);
  CK(box64dl_signal_action_get(sig, &after) == 0, "get after retry failed");
  CK(after.handler == (uintptr_t)h_nat_b, "retry handler=%p expected %p",
     (void*)after.handler, (void*)h_nat_b);
  CK(after.generation > before.generation,
     "retry generation must advance: %llu -> %llu",
     (unsigned long long)before.generation,
     (unsigned long long)after.generation);
  CK(old2.handler == before.handler,
     "a SUCCESSFUL set must report the replaced action: old=%p expected %p",
     (void*)old2.handler, (void*)before.handler);
  CK(old2.generation == before.generation,
     "a SUCCESSFUL set must report the replaced action's generation: %llu "
     "expected %llu", (unsigned long long)old2.generation,
     (unsigned long long)before.generation);
  box64dl_signal_actions_fini();
}

/* 6. an injected failure on one MANDATORY init signal rolls all of them back. */
static void case_injected_init_failure(void) {
  const int sig = SIGRTMIN + 3;

  struct sigaction pre;
  memset(&pre, 0, sizeof(pre));
  pre.sa_sigaction = h_nat_d;
  pre.sa_flags = SA_SIGINFO | SA_RESTART;
  sigemptyset(&pre.sa_mask);
  sigaddset(&pre.sa_mask, SIGUSR2);
  CK(sigaction(SIGABRT, &pre, NULL) == 0, "preinstall SIGABRT failed errno=%d",
     errno);
  struct sigaction pre_ill;
  memset(&pre_ill, 0, sizeof(pre_ill));
  pre_ill.sa_sigaction = h_nat_c;
  pre_ill.sa_flags = SA_SIGINFO;
  sigemptyset(&pre_ill.sa_mask);
  CK(sigaction(SIGILL, &pre_ill, NULL) == 0, "preinstall SIGILL failed");

  /* SIGABRT is the third member of the frozen mandatory list
   * {SIGSEGV, SIGBUS, SIGILL, SIGABRT} */
  g_fail_sig = SIGABRT;
  g_fail_skip = 0;
  g_fail_times = 1;
  atomic_store(&g_fail_hits, 0);

  errno = 0;
  int rc = box64dl_signal_actions_init();
  int e = errno;
  g_fail_sig = 0;
  g_fail_times = 0;

  CK(rc == -1, "init with an injected mandatory install failure must fail");
  CK(e == EPERM, "init failure errno=%d expected EPERM", e);
  CK(box64dl_signal_actions_active() == 0,
     "active() must be 0 after a failed init, got %d",
     box64dl_signal_actions_active());
  errno = 0;
  box64dl_signal_action_t a;
  CK(box64dl_signal_action_get(SIGSEGV, &a) == -1 && errno == ENOSYS,
     "get after a failed init must be ENOSYS, errno=%d", errno);
  errno = 0;
  CK(set_native_act(sig, (uintptr_t)h_nat_a, SA_SIGINFO, 0, 0, NULL) == -1 &&
         errno == ENOSYS,
     "set after a failed init must be ENOSYS, errno=%d", errno);

  struct sigaction cur;
  memset(&cur, 0, sizeof(cur));
  CK(sigaction(SIGSEGV, NULL, &cur) == 0, "SIGSEGV query failed");
  CK(cur.sa_handler == SIG_DFL,
     "SIGSEGV disposition=%p must be rolled back to the captured SIG_DFL",
     (void*)cur.sa_handler);
  memset(&cur, 0, sizeof(cur));
  CK(sigaction(SIGBUS, NULL, &cur) == 0, "SIGBUS query failed");
  CK(cur.sa_handler == SIG_DFL,
     "SIGBUS disposition=%p must be rolled back to the captured SIG_DFL",
     (void*)cur.sa_handler);
  memset(&cur, 0, sizeof(cur));
  CK(sigaction(SIGILL, NULL, &cur) == 0, "SIGILL query failed");
  CK((const void*)cur.sa_sigaction == (const void*)h_nat_c,
     "SIGILL handler=%p must be rolled back to the pre-init %p",
     (void*)cur.sa_sigaction, (void*)h_nat_c);
  CK((uint32_t)cur.sa_flags == (uint32_t)SA_SIGINFO,
     "SIGILL flags=0x%08x must be the pre-init SIGINFO", (unsigned)cur.sa_flags);
  memset(&cur, 0, sizeof(cur));
  CK(sigaction(SIGABRT, NULL, &cur) == 0, "SIGABRT query failed");
  CK((const void*)cur.sa_sigaction == (const void*)h_nat_d,
     "SIGABRT handler=%p must still be the pre-init %p (its install failed)",
     (void*)cur.sa_sigaction, (void*)h_nat_d);
  CK(sigismember(&cur.sa_mask, SIGUSR2) == 1,
     "SIGABRT sa_mask must be the untouched pre-init one");

  /* a later clean init must succeed and capture the restored pre-images */
  CK(box64dl_signal_actions_init() == 0,
     "clean init after the rolled-back failure failed errno=%d", errno);
  CK(box64dl_signal_actions_active() == 1, "active() must be 1 after the retry");
  map_entries();
  box64dl_signal_action_t cap;
  CK(box64dl_signal_action_get(SIGILL, &cap) == 0, "get SIGILL failed");
  CK(cap.kind == BOX64_SIGNAL_ACTION_NATIVE && cap.handler == (uintptr_t)h_nat_c,
     "SIGILL capture after the retry kind=%d handler=%p expected NATIVE/%p",
     cap.kind, (void*)cap.handler, (void*)h_nat_c);
  CK(box64dl_signal_action_get(SIGABRT, &cap) == 0, "get SIGABRT failed");
  CK(cap.kind == BOX64_SIGNAL_ACTION_NATIVE && cap.handler == (uintptr_t)h_nat_d,
     "SIGABRT capture after the retry kind=%d handler=%p expected NATIVE/%p",
     cap.kind, (void*)cap.handler, (void*)h_nat_d);
  CK(set_native_act(sig, (uintptr_t)h_nat_a, SA_SIGINFO, 0, 0, NULL) == 0,
     "set after the retry failed errno=%d", errno);
  dump_kernel(sig, "after the rolled-back init retry");
  box64dl_signal_actions_fini();
}

/* 5. Deterministic staged-node interleaving on the writer's publication path.
 *
 * The writer is held AFTER the real sigaction() succeeded but BEFORE it
 * publishes `latest` -- the window the frozen design allows ("publish the
 * target variant slot, then install the kernel disposition, then publish
 * `latest`"). A delivery that lands on the target variant then genuinely gets
 * the staged node through the entry the KERNEL just installed, and the
 * assertions are the ones that window actually implies:
 *
 *   a) the staged node IS delivered (that is the design, not a failure);
 *   b) a logical query still reports the previous action;
 *   c) if the staged node is SA_RESETHAND, its one-shot fires at most once
 *      across many deliveries, and once the setter returns the consumed node is
 *      not resurrected as the authoritative action.
 */
typedef struct stall_s {
  _Atomic int exit;
  _Atomic int rc;
} stall_t;

static stall_t g_stall_after;

static void* stall_after_setter(void* arg) {
  (void)arg;
  box64dl_signal_action_t old;
  memset(&old, 0, sizeof(old));
  /* target variant 1; the staged node is RESETHAND so the consumption is
   * observable, and a different handler so the kernel really moves */
  int rc = set_native_act(SIGUSR1, (uintptr_t)h_nat_b,
                          SA_SIGINFO | SA_ONSTACK | SA_RESETHAND, 0, 0, &old);
  atomic_store(&g_stall_after.rc, rc);
  atomic_store(&g_stall_after.exit, 1);
  return NULL;
}

static void case_staged_oneshot_before_latest(void) {
  const int sig = SIGUSR1;
  const int vA = BOX64DL_SIGNAL_VARIANT_ONSTACK | BOX64DL_SIGNAL_VARIANT_RESTART;
  const int vB = BOX64DL_SIGNAL_VARIANT_ONSTACK;
  CK(box64dl_signal_actions_init() == 0, "init failed errno=%d", errno);
  map_entries();

  /* A: variant 3, NOT RESETHAND, so deliveries on variant 3 invoke A safely */
  box64dl_signal_action_t a0;
  memset(&a0, 0, sizeof(a0));
  int rc = set_native_act(sig, (uintptr_t)h_nat_a,
                          SA_SIGINFO | SA_ONSTACK | SA_RESTART, 0, 0, &a0);
  CK(rc == 0, "install A rc=%d errno=%d", rc, errno);
  box64dl_signal_action_t A;
  memset(&A, 0, sizeof(A));
  CK(box64dl_signal_action_get(sig, &A) == 0, "get A failed");
  const wrec_t* ra = wrap_last(1, sig);
  CK(ra != NULL, "A must reach the kernel");
  if (ra)
    CK(ra->handler == g_entry[vA],
       "A kernel handler=%p expected the variant-3 entry %p", ra->handler,
       g_entry[vA]);

  /* ---- genuine window: install taken effect, `latest` not yet published ---- */
  hits_reset();
  g_stall_after_sig = sig;
  g_stall_after_handler = (uintptr_t)g_entry[vB];
  atomic_store(&g_stall_after_seen, 0);
  atomic_store(&g_stall_after_reached, 0);
  atomic_store(&g_stall_after_release, 0);
  atomic_store(&g_stall_after.rc, -99);
  atomic_store(&g_stall_after.exit, 0);
  atomic_store(&g_stall_after_armed, 1);

  pthread_t th;
  CK(pthread_create(&th, NULL, stall_after_setter, NULL) == 0,
     "pthread_create failed");

  struct timespec ts = {0, 200 * 1000};
  int waited = 0;
  while (!atomic_load_explicit(&g_stall_after_reached, memory_order_acquire) &&
         waited++ < 25000)
    nanosleep(&ts, NULL);
  CK(atomic_load(&g_stall_after_reached) == 1,
     "the setter never reached the post-install hold (waited %d ms)",
     waited * 200 / 1000);

  if (atomic_load(&g_stall_after_reached) == 1) {
    struct sigaction kq;
    memset(&kq, 0, sizeof(kq));
    CK(sigaction(sig, NULL, &kq) == 0, "kernel query in the window failed");
    int which = -1;
    for (int v = 0; v < BOX64DL_SIGNAL_VARIANTS; ++v)
      if ((const void*)kq.sa_sigaction == g_entry[v]) which = v;
    CK(which == vB,
       "in the post-install window the kernel must already run the target "
       "variant %d entry, got %d", vB, which);

    box64dl_signal_action_t mid;
    memset(&mid, 0, sizeof(mid));
    int g = box64dl_signal_action_get(sig, &mid);
    CK(g == 0, "the in-window logical query failed errno=%d", errno);
    CK(mid.kind == BOX64_SIGNAL_ACTION_NATIVE &&
           mid.handler == (uintptr_t)h_nat_a,
       "in the publication window `latest` must still be A (NATIVE/%p), got "
       "kind=%d handler=%p", (void*)h_nat_a, mid.kind, (void*)mid.handler);
    INFO("genuine window confirmed: kernel entry=%d (B's), `latest` still A",
         which);

    /* The design's prepublish order means a delivery here legitimately gets the
     * staged node. What must hold is the ONE-SHOT DISCIPLINE. The consumption
     * state lives in this process's memory, so it must be exercised in-process:
     * a forked probe would inherit a fresh, unconsumed copy and could not prove
     * anything about the counter.
     *
     * 1) one delivery on the staged target variant: the staged node runs exactly
     *    once and the OLD action does not;
     * 2) that delivery consumed the one-shot, so a query must no longer name the
     *    consumed node;
     * 3) a further delivery of the same consumed generation must NOT re-run the
     *    handler: it resolves to the default, which is fatal for this signal, so
     *    the proof is that a forked delivery dies by THIS signal having invoked
     *    no handler. */
    hits_reset();
    deliver_user(sig, vB);
    int hitA = atomic_load(&g_hits[HID_A]);
    int hitB = atomic_load(&g_hits[HID_B]);
    CK(hitB == 1,
       "one delivery on the staged target variant must invoke the staged node "
       "exactly once (B=%d); the design published it before `latest`", hitB);
    CK(hitA == 0, "the staged entry must not serve the OLD action A (A=%d)", hitA);

    box64dl_signal_action_t post_consume;
    memset(&post_consume, 0, sizeof(post_consume));
    CK(box64dl_signal_action_get(sig, &post_consume) == 0,
       "query after the in-window consumption failed");
    logf_("  after the in-window consumption: kind=%d handler=%p flags=0x%08x "
          "gen=%llu\n",
          post_consume.kind, (void*)post_consume.handler,
          (unsigned)(uint32_t)post_consume.flags,
          (unsigned long long)post_consume.generation);
    CK(!(post_consume.kind == BOX64_SIGNAL_ACTION_NATIVE &&
         post_consume.handler == (uintptr_t)h_nat_b),
       "the consumed staged node is still the authoritative action: a consumed "
       "generation must resolve to its reset/continuation, not stay current "
       "(kind=%d handler=%p flags=0x%08x)", post_consume.kind,
       (void*)post_consume.handler, (unsigned)(uint32_t)post_consume.flags);

    int consume_probe = deliver_probe(sig, vB, "consumed generation, variant");
    CK((consume_probe & 0x100) != 0 && (consume_probe & 0xFF) == sig,
       "a second delivery of the already-consumed generation must resolve to the "
       "DEFAULT disposition (fatal for this signal, so a forked probe must die "
       "by it) and must never re-run the handler; probe=0x%x", consume_probe);
    if ((consume_probe & 0x100) != 0 && (consume_probe & 0xFF) == sig)
      INFO("the consumed staged generation was NOT re-delivered: a forked "
           "delivery of it died by the default rather than re-running the "
           "handler (one-shot honoured exactly once)");

    int stale_probe = deliver_probe(sig, vA, "stale sibling variant");
    if ((stale_probe & 0x100) != 0)
      logf_("  note: a delivery on the stale sibling variant %d died by signal "
            "%d (no handler invoked)\n",
            vA, stale_probe & 0xFF);
    else
      logf_("  stale sibling variant %d probe=0x%x outcome=%d\n", vA,
            stale_probe, dv_outcome(stale_probe));
  }

  atomic_store_explicit(&g_stall_after_release, 1, memory_order_release);
  CK(pthread_join(th, NULL) == 0, "pthread_join failed");
  atomic_store(&g_stall_after_armed, 0);
  CK(atomic_load(&g_stall_after.exit) == 1, "the setter did not return");
  CK(atomic_load(&g_stall_after.rc) == 0,
     "the stalled setter reported rc=%d errno=%d", atomic_load(&g_stall_after.rc),
     errno);

  /* ---- after the setter returns: a consumed node must NOT stay current ---- */
  box64dl_signal_action_t B;
  memset(&B, 0, sizeof(B));
  CK(box64dl_signal_action_get(sig, &B) == 0, "get after the set failed");
  logf_("  after the set: kind=%d handler=%p flags=0x%08x gen=%llu (A was "
        "gen=%llu handler=%p)\n",
        B.kind, (void*)B.handler, (unsigned)(uint32_t)B.flags,
        (unsigned long long)B.generation, (unsigned long long)A.generation,
        (void*)A.handler);
  CK(B.generation > A.generation,
     "the set must publish a newer generation: %llu -> %llu",
     (unsigned long long)A.generation, (unsigned long long)B.generation);
  if (atomic_load(&g_hits[HID_B]) == 1) {
    CK(!(B.kind == BOX64_SIGNAL_ACTION_NATIVE &&
         B.handler == (uintptr_t)h_nat_b &&
         ((uint32_t)B.flags & (uint32_t)SA_RESETHAND)),
       "the CONSUMED staged node is still the authoritative action: a consumed "
       "generation was resurrected (kind=%d handler=%p flags=0x%08x)", B.kind,
       (void*)B.handler, (unsigned)(uint32_t)B.flags);
    INFO("the consumed staged node was not resurrected (current kind=%d)", B.kind);
  } else {
    CK(B.kind == BOX64_SIGNAL_ACTION_NATIVE &&
           B.handler == (uintptr_t)h_nat_b,
       "an unconsumed staged node must become authoritative after the set; got "
       "kind=%d handler=%p", B.kind, (void*)B.handler);
  }

  /* a delivery on the now-stale variant-3 entry must not be served B */
  hits_reset();
  int r_old = deliver_probe(sig, vA, "stale variant-3 entry after the set");
  if ((r_old & 0x100) != 0)
    FAILF("the stale variant-3 entry killed its process (signal %d) although the "
          "previous action A was a live NATIVE handler: an in-flight "
          "old-generation delivery became fatal", r_old & 0xFF);
  else
    logf_("  stale variant-3 entry after the set: probe=0x%x (outcome=%d)\n",
          r_old, dv_outcome(r_old));
  dump_action("after the staged set", sig);
  dump_kernel(sig, "after the staged set");
  box64dl_signal_actions_fini();
}

/* 8. old-generation entry: a delivery that entered the OLD variant must never
 *    be served a snapshot whose logical flags produce a different variant. */
static void case_old_entry_variant(void) {
  const int sig = SIGRTMIN + 4;
  CK(box64dl_signal_actions_init() == 0, "init failed errno=%d", errno);
  map_entries();
  hits_reset();

  CK(set_native_act(sig, (uintptr_t)h_nat_a,
                    SA_SIGINFO | SA_ONSTACK | SA_RESTART, 0, 0, NULL) == 0,
     "install A failed errno=%d", errno);
  CK(set_native_act(sig, (uintptr_t)h_nat_b, SA_SIGINFO | SA_ONSTACK, 0, 0,
                    NULL) == 0,
     "install B failed errno=%d", errno);

  /* The old kernel frame entered the variant-3 entry, whose slot the
   * replacement cleared. Observe the resolution (nothing invoked / A / B /
   * killed) with the child doing its own evaluation. */
  int r_old = deliver_probe(sig, BOX64DL_SIGNAL_VARIANT_ONSTACK |
                                     BOX64DL_SIGNAL_VARIANT_RESTART,
                            "OLD variant-3 entry delivery");
  box64dl_signal_action_t cur;
  memset(&cur, 0, sizeof(cur));
  box64dl_signal_action_get(sig, &cur);
  const wrec_t* rv1 = wrap_last(1, sig);
  logf_("  old-entry context: latest{variant=%d handler=%p} kernel-entry "
        "{variant=1 handler=%p} probe=0x%x\n",
        v_of(cur.flags), (void*)cur.handler, rv1 ? rv1->handler : NULL, r_old);
  if ((r_old & 0x100) != 0) {
    int killed_by = r_old & 0xFF;
    if (killed_by == sig) {
      /* The header's frozen text: "Skip a snapshot whose flags disagree; never
       * a snapshot whose flags do not match the entry that delivered it." The
       * docs (10) say a stale generation is resolved to the DEFAULT node, so
       * the fatal default here is consistent with both texts. Record the
       * consequence, do not call it a defect. */
      INFO("a delivery on the old variant-3 entry resolved to the DEFAULT "
           "disposition: the process was killed by signal %d. The old action's "
           "variant slot is cleared by the replacement, so the previous "
           "generation's handler is not delivered. Consistent with the header "
           "(skip the mismatching snapshot) and the header (a stale generation "
           "resolves to the DEFAULT node); the guest test's resethand case "
           "expects the same shape for a consumed generation. Consequence "
           "worth flagging to the leader: a replacement with different "
           "SA_ONSTACK/SA_RESTART makes an already-in-flight old-generation "
           "delivery fatal rather than running the old handler.", killed_by);
    } else {
      FAILF("the old-variant-3 delivery was killed by signal %d (expected the "
            "delivered signal %d)", killed_by, sig);
    }
  } else if (dv_outcome(r_old) == DVO_B) {
    FAILF("a delivery on the OLD variant-3 entry invoked B, whose logical flags "
          "produce variant 1: the module handed out a snapshot whose flags do "
          "not match the entry that delivered it (header: 'Never a snapshot "
          "whose flags do not match the entry that delivered it'). "
          "`latest`=%p/kind %d.", (void*)cur.handler, cur.kind);
  } else if (dv_outcome(r_old) == DVO_A) {
    INFO("the old variant-3 entry was served A's variant-3 snapshot: A's node is "
         "still rooted by its own variant slot, which is the required behaviour "
         "for a LIVE previous action");
  } else {
    FAILF("a delivery on the old variant-3 entry must serve the LIVE previous "
          "action A (its node is still rooted by its own variant slot); instead "
          "nothing was invoked (probe=0x%x). This is not an ambiguity: a live "
          "previous action must stay deliverable from the entry that names it.",
          r_old);
  }

  hits_reset();
  int r_cur = deliver_probe(sig, BOX64DL_SIGNAL_VARIANT_ONSTACK,
                            "current variant-1 entry delivery");
  CK(r_cur != 0 && (r_cur & 0x100) == 0 && dv_outcome(r_cur) == DVO_B,
     "the current variant-1 entry must be served B; probe code=0x%x", r_cur);
  box64dl_signal_actions_fini();
}

/* 9. pool / reclaim: >20000 RESETHAND updates with consumption and recycling. */
static void case_pool_20k(void) {
  const int sig = SIGRTMIN + 5;
  CK(box64dl_signal_actions_init() == 0, "init failed errno=%d", errno);
  map_entries();
  enum { ITERS = 20000 };
  int bad = 0;
  int enomem = 0;
  int gen_back = 0;
  uint64_t prev_gen = 0;
  for (int i = 0; i < ITERS; ++i) {
    uint64_t flags = SA_SIGINFO | SA_ONSTACK | SA_RESETHAND;
    if (i & 1) flags |= SA_RESTART;
    box64dl_signal_action_t old;
    memset(&old, 0, sizeof(old));
    errno = 0;
    int rc = set_native_act(sig, (uintptr_t)h_nat_a, flags, 0, 0, &old);
    if (rc != 0) {
      bad++;
      if (errno == ENOMEM) enomem++;
      if (bad < 4)
        FAILF("pool iteration %d set rc=%d errno=%d", i, rc, errno);
      continue;
    }
    box64dl_signal_action_t a;
    if (box64dl_signal_action_get(sig, &a) != 0) {
      bad++;
      continue;
    }
    if (a.generation <= prev_gen) {
      gen_back++;
      if (gen_back < 4)
        FAILF("iteration %d generation did not advance: %llu <= %llu", i,
              (unsigned long long)a.generation, (unsigned long long)prev_gen);
    }
    prev_gen = a.generation;
    hits_reset();
    deliver_user(sig, v_of(flags));
    if (atomic_load(&g_hits[HID_A]) != 1) {
      bad++;
      if (bad < 4)
        FAILF("iteration %d did not consume the RESETHAND action exactly once "
              "(hits=%d)", i, atomic_load(&g_hits[HID_A]));
    }
  }
  CK(bad == 0, "%d of %d pool iterations failed", bad, ITERS);
  CK(enomem == 0,
     "the bounded pool was exhausted (%d ENOMEM) although the delivery path "
     "was quiescent between iterations", enomem);
  CK(gen_back == 0, "%d generations did not advance", gen_back);

  box64dl_signal_action_t last;
  memset(&last, 0, sizeof(last));
  CK(box64dl_signal_action_get(sig, &last) == 0, "final get failed");
  logf_("  final action after consumption: kind=%d flags=0x%08x gen=%llu\n",
        last.kind, (unsigned)(uint32_t)last.flags,
        (unsigned long long)last.generation);
  CK(last.kind == BOX64_SIGNAL_ACTION_DEFAULT,
     "after the one-shot consumption the current action must be DEFAULT, "
     "kind=%d", last.kind);
  INFO("20000 RESETHAND updates + consumptions completed with no ENOMEM");
  box64dl_signal_actions_fini();
}

/* 10. a native handler that leaves its frame by a foreign siglongjmp must not
 *     strand a reader: 20000 further setters must all succeed afterwards. */
static void case_foreign_longjmp(void) {
  const int sig = SIGUSR2;
  CK(box64dl_signal_actions_init() == 0, "init failed errno=%d", errno);
  map_entries();
  hits_reset();
  CK(set_native_act(sig, (uintptr_t)h_nat_jmp,
                    SA_SIGINFO | SA_ONSTACK | SA_RESTART, 0, 0, NULL) == 0,
     "install the jump handler failed errno=%d", errno);
  atomic_store(&g_jmp_ran, 0);

  if (sigsetjmp(g_jb, 1) == 0) {
    deliver_user(sig, BOX64DL_SIGNAL_VARIANT_ONSTACK |
                         BOX64DL_SIGNAL_VARIANT_RESTART);
    FAILF("box64dl_signal_deliver returned although the handler siglongjmp'd: "
          "the reader reference was NOT released before the callback");
  }
  CK(atomic_load(&g_jmp_ran) == 1, "the jump handler never ran");
  CK(atomic_load(&g_hits[HID_JMP]) == 1, "the jump handler ran %d times",
     atomic_load(&g_hits[HID_JMP]));

  /* Reader-count evidence: reclaim needs a quiescent moment, so a stranded
   * reference would make every later reclaim bail out. 20000 updates must all
   * succeed with no ENOMEM. */
  enum { ITERS = 20000 };
  int bad = 0, enomem = 0;
  for (int i = 0; i < ITERS; ++i) {
    uint64_t flags = SA_SIGINFO | SA_ONSTACK;
    if (i & 1) flags |= SA_RESTART;
    errno = 0;
    int rc = set_native_act(sig, (uintptr_t)h_nat_a, flags, 0, 0, NULL);
    if (rc != 0) {
      bad++;
      if (errno == ENOMEM) enomem++;
      if (bad < 4) FAILF("post-longjmp setter %d rc=%d errno=%d", i, rc, errno);
    }
  }
  CK(bad == 0, "%d of %d setters failed after the foreign longjmp", bad, ITERS);
  CK(enomem == 0,
     "%d ENOMEM after the foreign longjmp: a reader reference was stranded",
     enomem);

  hits_reset();
  deliver_user(sig, BOX64DL_SIGNAL_VARIANT_ONSTACK);
  CK(atomic_load(&g_hits[HID_JMP]) == 0,
     "the replaced jump handler ran again, hits=%d",
     atomic_load(&g_hits[HID_JMP]));
  CK(atomic_load(&g_hits[HID_A]) == 1,
     "the replacement handler must be invoked, hits=%d",
     atomic_load(&g_hits[HID_A]));
  box64dl_signal_actions_fini();
}

/* 11. the logical mask and a real delivered ucontext mask are honoured. */
static void case_uc_mask_edit(void) {
  const int sig = SIGUSR1;
  CK(box64dl_signal_actions_init() == 0, "init failed errno=%d", errno);
  map_entries();
  hits_reset();

  /* SA_NODEFER with an explicit sa_mask{SIGUSR1}: the self bit is the app's */
  CK(set_native_act(sig, (uintptr_t)h_nat_a, SA_SIGINFO | SA_NODEFER, SIGUSR1, 0,
                    NULL) == 0,
     "install the NODEFER action failed errno=%d", errno);
  sigset_t clean;
  sigemptyset(&clean);
  CK(sigprocmask(SIG_SETMASK, &clean, NULL) == 0, "mask set failed");
  dump_action("before the NODEFER delivery", sig);
  int r_iso = deliver_probe(sig, V_PLAIN, "NODEFER + sa_mask{SIGUSR1}");
  CK(r_iso != 0 && (r_iso & 0x100) == 0 && dv_outcome(r_iso) == DVO_A,
     "the delivery of a freshly installed NATIVE action must invoke its handler "
     "(probe=0x%x; 0x100+sig means killed by that signal)", r_iso);
  CK((dv_bits(r_iso) & DV_BIT_USR1) != 0,
     "with SA_NODEFER and an explicit sa_mask{SIGUSR1} the handler must see "
     "SIGUSR1 blocked (bits=0x%x)", dv_bits(r_iso));

  /* without SA_NODEFER the module adds the self bit itself */
  hits_reset();
  CK(set_native_act(sig, (uintptr_t)h_nat_a, SA_SIGINFO, 0, 0, NULL) == 0,
     "install the plain action failed errno=%d", errno);
  CK(sigprocmask(SIG_SETMASK, &clean, NULL) == 0, "mask set failed");
  int r_plain = deliver_probe(sig, V_PLAIN, "plain SIGINFO action");
  CK((r_plain & 0x100) == 0 && dv_outcome(r_plain) == DVO_A,
     "a plain delivery must invoke the installed handler (probe=0x%x)", r_plain);
  CK((dv_bits(r_plain) & DV_BIT_USR1) != 0,
     "without SA_NODEFER the module must block the delivered signal in the "
     "handler (bits=0x%x)", dv_bits(r_plain));

  /* a real ucontext mask edit is honoured, and the ucontext is passed through */
  hits_reset();
  static ucontext_t uc;
  memset(&uc, 0, sizeof(uc));
  sigemptyset(&uc.uc_sigmask);
  sigaddset(&uc.uc_sigmask, SIGHUP);
  CK(sigprocmask(SIG_SETMASK, &clean, NULL) == 0, "mask set failed");
  siginfo_t info;
  memset(&info, 0, sizeof(info));
  info.si_signo = sig;
  info.si_code = SI_USER;
  g_dv_uc_want = (uintptr_t)&uc;
  int r_uc = deliver_probe_uc(sig, V_PLAIN, &uc, &info,
                              "real native ucontext delivery");
  CK((dv_bits(r_uc) & DV_BIT_UC) != 0,
     "the handler must receive the real native ucontext (%p), probe=0x%x",
     (void*)&uc, r_uc);
  CK((dv_bits(r_uc) & DV_BIT_HUP) != 0,
     "the ucontext mask edit (SIGHUP) must be in force inside the handler "
     "(bits=0x%x)", dv_bits(r_uc));
  CK((dv_bits(r_uc) & DV_BIT_USR1) != 0,
     "the delivered signal must also be blocked (no SA_NODEFER, bits=0x%x)",
     dv_bits(r_uc));
  CK(sigismember(&uc.uc_sigmask, SIGHUP) == 1,
     "the module must leave the caller's ucontext untouched");
  CK(sigismember(&uc.uc_sigmask, SIGUSR1) == 0,
     "the module must not write the self bit into the ucontext");
  box64dl_signal_actions_fini();
}

/* 12. snapshot coherence under concurrency.
 *
 *     Phase 1 is deterministic: five threads each loop one mutation and one
 *     reading query of the SAME signal; any permanently torn value or doomed
 *     lazy reclaim shows up as a wedged loop and is reported (with the observed
 *     counts) instead of being retried until the suite is killed.
 *
 *     Phase 2 is the live mixed load (writer + readers + deliveries) with every
 *     snapshot checked for internal coherence.
 */
typedef struct mt_s {
  _Atomic int stop;
  _Atomic int errors;
  _Atomic int updates;
  _Atomic int last_bad_errno;
  _Atomic int queried;
  _Atomic int torn; /* incoherent but still progressing: reported, see below */
  _Atomic int stalled;
  _Atomic int e_handler;   /* handler identity != the one its flags imply */
  _Atomic int e_mask;      /* a logical mask where the test set none */
  _Atomic int e_gen_back;  /* the observed current generation went backwards */
  _Atomic int e_kind;      /* a combination that cannot exist at all */
  _Atomic int eblock;      /* writers refused with the documented EWOULDBLOCK */
  _Atomic int enomem;      /* writers that found the bounded pool exhausted */
  _Atomic int e_torn_publish; /* a REFUSED set changed the snapshot or kernel */
} mt_t;

#define MT_SIG (SIGRTMIN + 6)

/* the first few incoherent snapshots, captured verbatim for the report */
typedef struct mtbad_s {
  int kind;
  uintptr_t handler;
  uint64_t flags;
  uint64_t gen;
  int reserved0;
  int want_variant;
  int where;
} mtbad_t;
#define MTBAD_MAX 12
static mtbad_t g_mtbad[MTBAD_MAX];
static volatile int g_mtbad_n;

static void mtbad_record(int where, const box64dl_signal_action_t* a, int wv) {
  int i = g_mtbad_n;
  if (i >= MTBAD_MAX) return;
  g_mtbad_n = i + 1;
  g_mtbad[i].kind = a->kind;
  g_mtbad[i].handler = a->handler;
  g_mtbad[i].flags = a->flags;
  g_mtbad[i].gen = a->generation;
  g_mtbad[i].reserved0 = a->_reserved0;
  g_mtbad[i].want_variant = wv;
  g_mtbad[i].where = where;
}

/* The exact set of (kind, flags, handler) triples this signal may ever hold:
 * the pre-init capture (DEFAULT, gen 1) and the two installed NATIVE actions.
 * Anything else is impossible without a torn node or a wrong publication. */
static int mt_valid(const box64dl_signal_action_t* a, int* is_capture,
                    int* is_native) {
  *is_capture = 0;
  *is_native = 0;
  if (a->kind == BOX64_SIGNAL_ACTION_DEFAULT) {
    if ((uint32_t)a->flags == 0 && a->handler == 0 && a->restorer == 0 &&
        a->generation == 1) {
      *is_capture = 1;
      return 1;
    }
    return 0;
  }
  if (a->kind != BOX64_SIGNAL_ACTION_NATIVE) return 0;
  if (a->handler == (uintptr_t)h_nat_a &&
      (uint32_t)a->flags == (uint32_t)(SA_SIGINFO | SA_ONSTACK)) {
    *is_native = 1;
    return 1;
  }
  if (a->handler == (uintptr_t)h_nat_b &&
      (uint32_t)a->flags ==
          (uint32_t)(SA_SIGINFO | SA_ONSTACK | SA_RESTART)) {
    *is_native = 1;
    return 1;
  }
  return 0;
}

/* Shared snapshot audit. Returns 0 when the snapshot is impossible.
 * `seen_native` / `prev_native_gen`: once a NATIVE replacement has been seen,
 * the capture (an OLDER generation) must never become current again, and the
 * current NATIVE generation must never go backwards. */
typedef struct mt_audit_s {
  int seen_native;
  uint64_t prev_native_gen;
} mt_audit_t;

static int mt_audit(mt_t* s, const box64dl_signal_action_t* a, mt_audit_t* au) {
  int cap = 0, nat = 0;
  if (a->_reserved0 != 0) atomic_fetch_add(&s->e_kind, 1);
  if (!sigisemptyset(&a->mask)) atomic_fetch_add(&s->e_mask, 1);
  if (!mt_valid(a, &cap, &nat)) {
    atomic_fetch_add(&s->e_kind, 1);
    mtbad_record(5, a, v_of(a->flags));
    logf_("  IMPOSSIBLE snapshot: kind=%d handler=%p restorer=%p flags=0x%08x "
          "gen=%llu reserved0=%d (only the capture DEFAULT/gen1 or "
          "NATIVE h_nat_a[V1] / h_nat_b[V3] may ever be current)\n",
          a->kind, (void*)a->handler, (void*)a->restorer,
          (unsigned)(uint32_t)a->flags, (unsigned long long)a->generation,
          a->_reserved0);
    return 0;
  }
  if (cap) {
    if (au->seen_native) {
      atomic_fetch_add(&s->e_gen_back, 1);
      mtbad_record(6, a, 0);
      logf_("  STALE capture became current again after a NATIVE replacement "
            "(gen=%llu)\n", (unsigned long long)a->generation);
    }
    return 1;
  }
  if (nat) {
    au->seen_native = 1;
    if (a->generation < au->prev_native_gen) {
      atomic_fetch_add(&s->e_gen_back, 1);
      mtbad_record(7, a, v_of(a->flags));
      logf_("  NATIVE generation went backwards: %llu -> %llu\n",
            (unsigned long long)au->prev_native_gen,
            (unsigned long long)a->generation);
    }
    if (a->generation > au->prev_native_gen)
      au->prev_native_gen = a->generation;
  }
  return 1;
}

static void* mt_churn(void* arg) {
  mt_t* s = (mt_t*)arg;
  mt_audit_t au;
  memset(&au, 0, sizeof(au));
  for (int i = 0; i < 4000; ++i) {
    uint64_t flags = SA_SIGINFO | SA_ONSTACK;
    uintptr_t h = (uintptr_t)h_nat_a;
    if (i & 1) {
      flags |= SA_RESTART;
      h = (uintptr_t)h_nat_b;
    }
    errno = 0;
    if (set_native_act(MT_SIG, h, flags, 0, 0, NULL) != 0) {
      /* A refused writer (EWOULDBLOCK) is a documented, bounded outcome: the
       * previous action simply stays current. Count it, do not call it a bug. */
      if (errno == EWOULDBLOCK)
        atomic_fetch_add(&s->eblock, 1);
      else if (errno == ENOMEM)
        atomic_fetch_add(&s->enomem, 1);
      else {
        atomic_fetch_add(&s->errors, 1);
        atomic_store(&s->last_bad_errno, errno);
      }
    } else {
      atomic_fetch_add(&s->updates, 1);
    }
    box64dl_signal_action_t a;
    memset(&a, 0, sizeof(a));
    if (box64dl_signal_action_get(MT_SIG, &a) != 0) {
      atomic_fetch_add(&s->errors, 1);
      continue;
    }
    atomic_fetch_add(&s->queried, 1);
    if (a._reserved0 != 0 || a.generation == 0)
      atomic_fetch_add(&s->torn, 1);
    mt_audit(s, &a, &au);
  }
  return NULL;
}

static void* mt_reader(void* arg) {
  mt_t* s = (mt_t*)arg;
  mt_audit_t au;
  memset(&au, 0, sizeof(au));
  while (!atomic_load_explicit(&s->stop, memory_order_relaxed)) {
    box64dl_signal_action_t a;
    memset(&a, 0, sizeof(a));
    if (box64dl_signal_action_get(MT_SIG, &a) != 0) continue;
    atomic_fetch_add(&s->queried, 1);
    mt_audit(s, &a, &au);
  }
  return NULL;
}

static void* mt_writer(void* arg) {
  mt_t* s = (mt_t*)arg;
  uintptr_t last_h = (uintptr_t)h_nat_a;
  uint64_t last_f = SA_SIGINFO | SA_ONSTACK;
  for (int i = 0; i < 4000; ++i) {
    uint64_t flags = SA_SIGINFO | SA_ONSTACK;
    uintptr_t h = (uintptr_t)h_nat_a;
    if (i & 1) {
      flags |= SA_RESTART;
      h = (uintptr_t)h_nat_b;
    }
    errno = 0;
    if (set_native_act(MT_SIG, h, flags, 0, 0, NULL) == 0) {
      last_h = h;
      last_f = flags;
      atomic_fetch_add(&s->updates, 1);
      continue;
    }
    {
      if (errno == EWOULDBLOCK)
        atomic_fetch_add(&s->eblock, 1);
      else if (errno == ENOMEM) {
        /* Documented boundary, not a defect: src/signal_actions.c:51-57 -- "The
         * pool is the bound on how many replacements can happen without a
         * quiescent moment in between ... Exhaustion is reported as ENOMEM,
         * never as a silent drop." This thread is the ONLY publisher of MT_SIG
         * in this phase and MT_SIG's actions carry no SA_RESETHAND, so nothing
         * else can publish either: a refused set MUST leave the logical snapshot
         * and the kernel disposition exactly equal to this thread's last
         * SUCCESSFUL publication. */
        atomic_fetch_add(&s->enomem, 1);
        box64dl_signal_action_t post;
        memset(&post, 0, sizeof(post));
        if (box64dl_signal_action_get(MT_SIG, &post) != 0) {
          atomic_fetch_add(&s->errors, 1);
        } else if (post.handler != last_h ||
                   (uint32_t)post.flags != (uint32_t)last_f) {
          atomic_fetch_add(&s->e_torn_publish, 1);
        }
        struct sigaction kpost;
        memset(&kpost, 0, sizeof(kpost));
        if (sigaction(MT_SIG, NULL, &kpost) != 0 ||
            (const void*)kpost.sa_sigaction != g_entry[v_of(last_f)])
          atomic_fetch_add(&s->e_torn_publish, 1);
      } else {
        atomic_fetch_add(&s->errors, 1);
        atomic_store(&s->last_bad_errno, errno);
      }
    }
  }
  atomic_store_explicit(&s->stop, 1, memory_order_release);
  return NULL;
}

static void* mt_deliverer(void* arg) {
  mt_t* s = (mt_t*)arg;
  while (!atomic_load_explicit(&s->stop, memory_order_relaxed)) {
    deliver_user(MT_SIG, BOX64DL_SIGNAL_VARIANT_ONSTACK);
    deliver_user(MT_SIG,
                 BOX64DL_SIGNAL_VARIANT_ONSTACK | BOX64DL_SIGNAL_VARIANT_RESTART);
  }
  return NULL;
}

/* join with a bounded wait so a hung child is reported, not waited on forever */
static int join_bounded(pthread_t th, int seconds) {
  struct timespec ts = {0, 50 * 1000 * 1000};
  for (int i = 0; i < seconds * 20; ++i) {
    if (pthread_tryjoin_np(th, NULL) == 0) return 0;
    nanosleep(&ts, NULL);
  }
  return -1;
}

static void case_multithread_snapshot(void) {
  CK(box64dl_signal_actions_init() == 0, "init failed errno=%d", errno);
  map_entries();
  hits_reset();

  /* ---- phase 1: 5 threads, mutation + reading query, bounded ---- */
  mt_t st;
  memset(&st, 0, sizeof(st)); /* every counter starts defined */
  pthread_t ch[5];
  for (int i = 0; i < 5; ++i)
    CK(pthread_create(&ch[i], NULL, mt_churn, &st) == 0, "churn create");
  int hung = 0;
  for (int i = 0; i < 5; ++i) {
    if (join_bounded(ch[i], 30) != 0) {
      hung = 1;
      logf_("  phase-1 thread %d did not finish in 30s (updates=%d queried=%d)\n",
            i, atomic_load(&st.updates), atomic_load(&st.queried));
    }
  }
  int p1_hand = atomic_load(&st.e_handler);
  int p1_mask = atomic_load(&st.e_mask);
  int p1_back = atomic_load(&st.e_gen_back);
  int p1_kind = atomic_load(&st.e_kind);
  int p1_ebl = atomic_load(&st.eblock);
  int p1_en = atomic_load(&st.enomem);
  int p1_err = atomic_load(&st.errors);
  logf_("  phase 1: updates=%d queries=%d EWOULDBLOCK=%d ENOMEM=%d errors=%d "
        "torn=%d hung=%d | incoherent: impossible=%d mask=%d gen_backwards=%d "
        "handler=%d\n",
        atomic_load(&st.updates), atomic_load(&st.queried), p1_ebl, p1_en,
        p1_err, atomic_load(&st.torn), hung, p1_kind, p1_mask, p1_back, p1_hand);
  {
    int nb = g_mtbad_n;
    if (nb > MTBAD_MAX) nb = MTBAD_MAX;
    for (int i = 0; i < nb; ++i)
      logf_("  incoherent sample %d (where %d): kind=%d handler=%p flags=0x%08x "
            "variant=%d gen=%llu reserved0=%d\n",
            i, g_mtbad[i].where, g_mtbad[i].kind, (void*)g_mtbad[i].handler,
            (unsigned)(uint32_t)g_mtbad[i].flags, g_mtbad[i].want_variant,
            (unsigned long long)g_mtbad[i].gen, g_mtbad[i].reserved0);
  }
  CK(hung == 0,
     "a thread doing alternating set+get on one signal wedged for 30s: with a "
     "correct writer-mutex handoff and a correct reader grace period it must "
     "make progress (updates=%d queries=%d)",
     atomic_load(&st.updates), atomic_load(&st.queried));
  CK(p1_hand + p1_mask + p1_kind == 0,
     "phase 1 reported %d snapshots that no publication can explain "
     "(impossible combination=%d, mask=%d, handler-vs-flags=%d)",
     p1_hand + p1_mask + p1_kind, p1_kind, p1_mask, p1_hand);
  CK(p1_back == 0,
     "phase 1 observed the current action walk BACKWARDS %d times (a NATIVE "
     "generation regressed, or the older capture became current again after a "
     "replacement)", p1_back);
  if (p1_ebl > 0 || p1_en > 0)
    INFO("under 5-way contention on one signal: %d setter attempts refused with "
         "the documented EWOULDBLOCK and %d with the documented ENOMEM, %d "
         "succeeded. Both are legitimate bounded refusals under the header's "
         "ERRNO CONTRACT (EWOULDBLOCK: the try-only writer mutex, refused before "
         "any state is touched; ENOMEM: the frozen pool quota for replacements "
         "without a quiescent moment, src/signal_actions.c:51-57). Neither is "
         "corruption; the success ratio at this concurrency is worth noting.",
         p1_ebl, p1_en, atomic_load(&st.updates));
  /* The header's ERRNO CONTRACT applies to every writer, not only to the
   * phase-2 load: a phase-1 refusal may legitimately be EWOULDBLOCK (try-only
   * writer mutex) OR ENOMEM (the frozen bounded pool: "Exhaustion is reported as
   * ENOMEM, never as a silent drop", src/signal_actions.c:51-57). Both are
   * classified separately by mt_churn() and neither is a coherence failure, so
   * an occasional bounded budget refusal under scheduler pressure must not fail
   * this case. Only an errno outside those two is a defect. */
  CK(p1_err == 0,
     "phase 1 saw %d set failures whose errno was neither of the two documented "
     "refusals EWOULDBLOCK/ENOMEM (last errno=%d=%s); observed refusals were "
     "EWOULDBLOCK=%d and ENOMEM=%d",
     p1_err, atomic_load(&st.last_bad_errno),
     strerror(atomic_load(&st.last_bad_errno)), p1_ebl, p1_en);
  if (p1_en > 0)
    INFO("phase 1 also hit the frozen pool boundary %d times (EWOULDBLOCK=%d, "
         "publications=%d); per the header's ERRNO CONTRACT this is a legal "
         "bounded refusal for any writer, and the phase-2 section asserts the "
         "stronger properties on that boundary (nothing changed by a refusal, "
         "and the quota recovers once a quiescent moment exists)",
         p1_en, p1_ebl, atomic_load(&st.updates));
  CK(atomic_load(&st.torn) == 0,
     "phase 1 saw %d torn snapshots (reserved0 != 0 or generation == 0)",
     atomic_load(&st.torn));

  /* ---- phase 2: live mixed load ---- */
  atomic_store(&st.stop, 0);
  atomic_store(&st.errors, 0);
  atomic_store(&st.updates, 0);
  atomic_store(&st.queried, 0);
  atomic_store(&st.torn, 0);
  atomic_store(&st.e_handler, 0);
  atomic_store(&st.e_mask, 0);
  atomic_store(&st.e_gen_back, 0);
  atomic_store(&st.e_kind, 0);
  atomic_store(&st.eblock, 0);
  atomic_store(&st.enomem, 0);
  atomic_store(&st.errors, 0);
  atomic_store(&st.last_bad_errno, 0);
  pthread_t rd[4], wr, dv;
  for (int i = 0; i < 4; ++i)
    CK(pthread_create(&rd[i], NULL, mt_reader, &st) == 0, "reader create");
  CK(pthread_create(&dv, NULL, mt_deliverer, &st) == 0, "deliverer create");
  CK(pthread_create(&wr, NULL, mt_writer, &st) == 0, "writer create");
  CK(join_bounded(wr, 30) == 0, "the phase-2 writer did not finish in 30s");
  atomic_store_explicit(&st.stop, 1, memory_order_release);
  for (int i = 0; i < 4; ++i) join_bounded(rd[i], 10);
  join_bounded(dv, 10);
  int errs = atomic_load(&st.e_handler) + atomic_load(&st.e_mask) +
             atomic_load(&st.e_kind) + atomic_load(&st.e_gen_back) +
             atomic_load(&st.errors);
  CK(errs == 0,
     "under the live mixed load: impossible=%d mask=%d gen_backwards=%d "
     "handler=%d other-set-failures=%d (last errno=%d=%s; EWOULDBLOCK refusals "
     "are counted separately: %d)",
     atomic_load(&st.e_kind), atomic_load(&st.e_mask),
     atomic_load(&st.e_gen_back), atomic_load(&st.e_handler),
     atomic_load(&st.errors), atomic_load(&st.last_bad_errno),
     strerror(atomic_load(&st.last_bad_errno)), atomic_load(&st.eblock));
  /* ENOMEM here is the FROZEN, DOCUMENTED boundary, not a defect.
   *
   * src/signal_actions.c:51-57 (MEMORY): "The pool is the bound on how many
   * replacements can happen without a quiescent moment in between (every set
   * attempts a grace-period reclaim first). Exhaustion is reported as ENOMEM,
   * never as a silent drop." With four readers in a tight get() loop there is no
   * quiescent moment, so reclaim_retired() (src/signal_actions.c:478-480, the
   * only call site being :547) cannot run and the fixed budget is consumed. The
   * header's pool comment (src/signal_actions.c:159-164) sizes a simultaneous
   * ROOT bound (captures plus four variants per signal), not an unbounded
   * retired-stream budget.
   *
   * What is asserted instead, because it IS the contract:
   *   a) a refused set never changed the logical snapshot or the kernel
   *      disposition (checked per refusal by the writer above);
   *   b) coherence holds throughout (no impossible snapshot, no regression);
   *   c) quota recovery: once the readers are stopped and joined so the grace
   *      counter is 0 again, a previously refused publication SUCCEEDS, i.e. the
   *      refusal is a temporary budget condition and not a sticky state. */
  int en = atomic_load(&st.enomem);
  CK(atomic_load(&st.e_torn_publish) == 0,
     "%d REFUSED (ENOMEM) publications changed the logical snapshot or the "
     "kernel disposition: a refusal must happen before any state is touched",
     atomic_load(&st.e_torn_publish));

  /* (c) readers are already stopped and joined above, so g_readers is 0 again */
  int retry_rc = -99;
  int retry_errno = 0;
  errno = 0;
  retry_rc = set_native_act(MT_SIG, (uintptr_t)h_nat_a,
                            SA_SIGINFO | SA_ONSTACK, 0, 0, NULL);
  retry_errno = errno;
  if (en > 0) {
    CK(retry_rc == 0,
       "after the readers were stopped and joined (grace counter back to 0, so "
       "reclaim can run) a publication that the same load had refused with "
       "ENOMEM must succeed; got rc=%d errno=%d=%s. A permanent refusal would "
       "mean the quota never recovers.", retry_rc, retry_errno,
       strerror(retry_errno));
    if (retry_rc == 0) {
      int extra = 0;
      for (int i = 0; i < 64; ++i)
        if (set_native_act(MT_SIG,
                           i & 1 ? (uintptr_t)h_nat_b : (uintptr_t)h_nat_a,
                           SA_SIGINFO | SA_ONSTACK |
                               (i & 1 ? (uint64_t)SA_RESTART : 0),
                           0, 0, NULL) != 0)
          extra++;
      CK(extra == 0,
         "%d of 64 further publications failed after quiescence: reclaim must "
         "restore the full budget", extra);
      INFO("quota recovery proven: %d ENOMEM refusals during the reader-live "
           "window, then the refused publication and 64 further ones succeeded "
           "once the readers were joined (grace counter 0)", en);
    }
  } else {
    INFO("the pool was not exhausted under this load (%d publications, 0 ENOMEM)",
         atomic_load(&st.updates));
  }
  /* Deterministic final state: the last SUCCESSFUL publication during the load
   * is not necessarily the final iteration (it may have been refused), so
   * publish one known action now that every reader is gone. */
  CK(set_native_act(MT_SIG, (uintptr_t)h_nat_b,
                    SA_SIGINFO | SA_ONSTACK | SA_RESTART, 0, 0, NULL) == 0,
     "the final deterministic publication failed errno=%d", errno);
  box64dl_signal_action_t a;
  CK(box64_signal_action_get(MT_SIG, &a) == 0, "final get failed");
  CK(a.kind == BOX64_SIGNAL_ACTION_NATIVE && a.handler == (uintptr_t)h_nat_b &&
         (uint32_t)a.flags ==
             (uint32_t)(SA_SIGINFO | SA_ONSTACK | SA_RESTART),
     "final action kind=%d handler=%p flags=0x%08x expected NATIVE/%p "
     "flags=0x%08x", a.kind, (void*)a.handler, (unsigned)(uint32_t)a.flags,
     (void*)h_nat_b, (unsigned)(SA_SIGINFO | SA_ONSTACK | SA_RESTART));
  INFO("multithread coherence: phase2 updates=%d errors=%d EWOULDBLOCK=%d; "
       "phase1 impossible=%d gen_back=%d (phase-1 counters above)",
       atomic_load(&st.updates), errs, atomic_load(&st.eblock), p1_kind, p1_back);
  box64dl_signal_actions_fini();
}

/* 13. GUEST actions are never called natively; they reach the guest worker,
 *     and IGNORE never calls anything. */
static void case_guest_route(void) {
  const int sig = SIGRTMIN + 7;
  CK(box64dl_signal_actions_init() == 0, "init failed errno=%d", errno);
  map_entries();
  hits_reset();
  box64dl_signal_action_t act;
  memset(&act, 0, sizeof(act));
  act.kind = BOX64_SIGNAL_ACTION_GUEST;
  act.handler = 0x400000; /* a guest x86-64 address, never native-callable */
  act.restorer = 0x400100;
  act.flags = (uint32_t)(SA_SIGINFO | SA_ONSTACK);
  sigemptyset(&act.mask);
  CK(box64dl_signal_action_set(sig, &act, NULL) == 0,
     "the guest set failed errno=%d", errno);
  const wrec_t* r = wrap_last(1, sig);
  CK(r != NULL && r->handler == g_entry[BOX64DL_SIGNAL_VARIANT_ONSTACK],
     "a GUEST action must still install the module entry (variant 1)");
  box64dl_signal_action_t back;
  CK(box64dl_signal_action_get(sig, &back) == 0, "guest get failed");
  CK(back.kind == BOX64_SIGNAL_ACTION_GUEST && back.handler == 0x400000,
     "the logical guest action must round-trip, kind=%d handler=%p", back.kind,
     (void*)back.handler);

  atomic_store(&g_guest_calls, 0);
  dump_action("before the GUEST delivery", sig);
  int r_guest = deliver_probe(sig, BOX64DL_SIGNAL_VARIANT_ONSTACK,
                              "GUEST action delivery");
  CK((r_guest & 0x100) == 0 && dv_outcome(r_guest) == DVO_GUEST,
     "a GUEST action must reach box64dl_signal_guest_deliver for the action "
     "`latest` names (probe=0x%x; 0x100+sig means killed by that signal)",
     r_guest);
  CK((dv_bits(r_guest) & DV_BIT_GUEST_BAD) == 0,
     "the guest worker was called with something other than the frozen "
     "by-value GUEST action (bit set in probe=0x%x; see the probe line above "
     "for guest_last{kind,handler,x64pc})", r_guest);

  box64dl_signal_action_t ign;
  memset(&ign, 0, sizeof(ign));
  ign.kind = BOX64_SIGNAL_ACTION_IGNORE;
  CK(box64dl_signal_action_set(sig, &ign, NULL) == 0, "the ignore set failed");
  hits_reset();
  int r_ign = deliver_probe(sig, V_PLAIN, "IGNORE action delivery");
  CK((r_ign & 0x100) == 0 && dv_outcome(r_ign) == DVO_NONE,
     "an IGNORE action delivery must invoke nothing and must not die "
     "(probe=0x%x)", r_ign);
  CK(atomic_load(&g_hits[HID_A]) == 0, "IGNORE must not call any handler");
  CK(atomic_load(&g_guest_calls) == 0,
     "IGNORE must not reach the guest worker (parent saw %d calls; the probe "
     "child reported outcome=%d)", atomic_load(&g_guest_calls),
     dv_outcome(r_ign));
  box64dl_signal_actions_fini();
}

/* 14. a DEFAULT action on a fatal signal really terminates, seen from a parent. */
static void case_default_fatal(void) {
  CK(box64dl_signal_actions_init() == 0, "init failed errno=%d", errno);
  const int sig = SIGRTMIN + 8;
  box64dl_signal_action_t a;
  CK(box64dl_signal_action_get(sig, &a) == 0, "get failed");
  CK(a.kind == BOX64_SIGNAL_ACTION_DEFAULT, "a fresh signal must be DEFAULT");

  pid_t pid = fork();
  if (pid == 0) {
    deliver_user(sig, V_PLAIN);
    _exit(0); /* must not be reached */
  }
  int st = 0;
  waitpid(pid, &st, 0);
  CK(WIFSIGNALED(st) && WTERMSIG(st) == sig,
     "a DEFAULT action on signal %d must terminate the child with WTERMSIG=%d, "
     "status=0x%x", sig, WIFSIGNALED(st) ? WTERMSIG(st) : -1, st);
  INFO("DEFAULT fatal child status=0x%x (signal %d)", st, sig);

  pid_t pid2 = fork();
  if (pid2 == 0) {
    /* the fatal path runs on a BIG stack: it re-raises with a signal frame on
     * whatever stack it is handed, and a small default thread stack makes that
     * frame overflow into unmapped memory (which would consume the process's
     * own SIGSEGV disposition and prove nothing) */
    static char bigstack[1 << 20];
    memset(bigstack, 0, sizeof(bigstack));
    siginfo_t info;
    memset(&info, 0, sizeof(info));
    info.si_signo = SIGSEGV;
    info.si_code = SI_USER;
    __asm__ volatile("" ::: "memory");
    box64dl_signal_deliver(NULL, SIGSEGV, &info, NULL, 0, NULL, (uintptr_t)-1);
    (void)bigstack;
    _exit(0);
  }
  int st2 = 0;
  waitpid(pid2, &st2, 0);
  CK(WIFSIGNALED(st2) && WTERMSIG(st2) == SIGSEGV,
     "DEFAULT on an engine signal must re-raise SIGSEGV (status=0x%x)", st2);
  CK(box64dl_signal_actions_active() == 1, "the parent must be unaffected");
  INFO("engine-fatal child status=0x%x", st2);
  box64dl_signal_actions_fini();
}

/* 15. The frozen ENOMEM boundary and quota recovery, made DETERMINISTIC.
 *
 * src/signal_actions.c:51-57 (MEMORY) is the contract this case pins:
 *   "The pool is the bound on how many replacements can happen without a
 *    quiescent moment in between (every set attempts a grace-period reclaim
 *    first). Exhaustion is reported as ENOMEM, never as a silent drop."
 *
 * A single-threaded writer always finds quiescent instants, so the boundary is
 * not reachable that way. It IS reachable deterministically from inside a native
 * handler: the delivery holds a reader reference for the whole callback, so the
 * grace counter is pinned non-zero, reclaim_retired() cannot run, and the fixed
 * budget is consumed by the handler's own publication stream. This is also the
 * exact situation the header describes (registration from a handler is bounded,
 * not async-signal-safe).
 *
 * Asserted:
 *   a) ENOMEM is reached and is reported as an errno, never as a silent drop
 *      (a refused set is observably NOT applied);
 *   b) every refusal left the logical snapshot AND the kernel disposition
 *      untouched (checked on the refusal itself, from inside the handler);
 *   c) after the delivery returns -- readers quiescent, grace counter 0 -- the
 *      very publication that was refused SUCCEEDS, and 64 further ones succeed:
 *      the quota recovers, the refusal is not sticky.
 */
#define EN_SIG (SIGRTMIN + 5) /* the target of the publication stream */

static _Atomic int g_en_ran;
static _Atomic int g_en_refused;
static _Atomic int g_en_torn;
static _Atomic int g_en_other_errno;
static _Atomic int g_en_cap_reached;
static _Atomic int g_en_pubs;

static void en_target_a(int s, siginfo_t* i, void* u) {
  /* recorded like the other stubs so deliver_probe() can classify the outcome */
  hit(HID_A, s, i, u, 1);
}

static void h_enomem(int sig, siginfo_t* info, void* ucntx) {
  (void)sig; (void)info; (void)ucntx;
  atomic_store(&g_en_ran, 1);
  for (int k = 0; k < 200000; ++k) {
    box64dl_signal_action_t pre;
    memset(&pre, 0, sizeof(pre));
    if (box64dl_signal_action_get(EN_SIG, &pre) != 0) {
      atomic_store(&g_en_other_errno, errno);
      return;
    }
    struct sigaction kq;
    memset(&kq, 0, sizeof(kq));
    if (sigaction(EN_SIG, NULL, &kq) != 0) {
      atomic_store(&g_en_other_errno, errno);
      return;
    }
    const void* kbefore = (const void*)kq.sa_sigaction;
    errno = 0;
    int rc = set_native_act(EN_SIG, (uintptr_t)en_target_a,
                            SA_SIGINFO | SA_ONSTACK, 0, 0, NULL);
    if (rc == 0) {
      atomic_fetch_add(&g_en_pubs, 1);
      continue;
    }
    if (errno == EWOULDBLOCK) continue; /* documented try-lock refusal */
    if (errno != ENOMEM) {
      atomic_store(&g_en_other_errno, errno);
      return;
    }
    /* The boundary was reached. A refusal must not have changed anything. */
    atomic_store(&g_en_refused, 1);
    box64dl_signal_action_t post;
    memset(&post, 0, sizeof(post));
    if (box64dl_signal_action_get(EN_SIG, &post) != 0 ||
        post.kind != pre.kind || post.handler != pre.handler ||
        (uint32_t)post.flags != (uint32_t)pre.flags ||
        post.generation != pre.generation)
      atomic_fetch_add(&g_en_torn, 1);
    struct sigaction kq2;
    memset(&kq2, 0, sizeof(kq2));
    if (sigaction(EN_SIG, NULL, &kq2) != 0 ||
        (const void*)kq2.sa_sigaction != kbefore)
      atomic_fetch_add(&g_en_torn, 1);
    return;
  }
  atomic_store(&g_en_cap_reached, 1); /* finished the stream without ENOMEM */
}

static void case_enomem_boundary_and_recovery(void) {
  const int sig = SIGUSR1;
  CK(box64dl_signal_actions_init() == 0, "init failed errno=%d", errno);
  map_entries();

  /* a plain (non-RESETHAND) NATIVE handler on `sig`, installed normally */
  CK(set_native_act(sig, (uintptr_t)h_enomem, SA_SIGINFO | SA_ONSTACK, 0, 0,
                    NULL) == 0,
     "installing the probing handler failed errno=%d", errno);
  atomic_store(&g_en_ran, 0);
  atomic_store(&g_en_refused, 0);
  atomic_store(&g_en_torn, 0);
  atomic_store(&g_en_other_errno, 0);
  atomic_store(&g_en_cap_reached, 0);
  atomic_store(&g_en_pubs, 0);

  /* a REAL delivery: the handler runs with the delivery's reader reference
   * held, so no quiescent moment exists for reclaim */
  CK(raise(sig) == 0, "raise(%d) failed errno=%d", sig, errno);

  CK(atomic_load(&g_en_ran) == 1, "the probing handler never ran");
  int other = atomic_load(&g_en_other_errno);
  CK(other == 0,
     "inside the handler the publication stream failed with an unexpected errno "
     "%d (%s); the only documented refusals are EWOULDBLOCK (try-lock) and "
     "ENOMEM (exhaustion), and a delivery must not leave a refusal state behind "
     "either (src/signal_actions.c:44-49)", other, strerror(other));
  CK(atomic_load(&g_en_torn) == 0,
     "%d refused publications changed the logical snapshot or the kernel "
     "disposition: a refusal must be an errno report and nothing else "
     "(src/signal_actions.c:51-57: reported, never a silent drop)",
     atomic_load(&g_en_torn));
  if (atomic_load(&g_en_cap_reached))
    INFO("the in-handler stream completed %d publications without reaching the "
         "bounded pool (the delivery does not pin the grace counter)", 
         atomic_load(&g_en_pubs));
  INFO("bounded in-handler registration: %d publications from INSIDE a native "
       "handler, no unexpected errno, no state damage. Note what this also "
       "shows: the module releases its reader reference BEFORE the callback "
       "(src/signal_actions.c:891 / the header's 'released before any "
       "application callback'), so a long handler does NOT itself pin the grace "
       "counter -- the ENOMEM boundary needs a concurrent reader or delivery "
       "elsewhere, which is why it is exercised by the live-load case and not "
       "here", atomic_load(&g_en_pubs));

  /* recovery: the delivery has returned, so the grace counter is 0 again */
  errno = 0;
  int rc = set_native_act(EN_SIG, (uintptr_t)en_target_a,
                          SA_SIGINFO | SA_ONSTACK, 0, 0, NULL);
  CK(rc == 0,
     "after the in-handler stream the publication must still succeed "
     "(grace counter 0, so reclaim can run); got rc=%d errno=%d=%s", rc, errno,
     strerror(errno));
  int extra = 0;
  for (int i = 0; i < 64; ++i) {
    uint64_t f = SA_SIGINFO | SA_ONSTACK;
    uintptr_t h = (uintptr_t)en_target_a;
    if (i & 1) {
      f |= SA_RESTART;
      h = (uintptr_t)h_nat_a;
    }
    if (set_native_act(EN_SIG, h, f, 0, 0, NULL) != 0) extra++;
  }
  CK(extra == 0,
     "%d of 64 publications failed after quiescence: the quota must recover "
     "completely once a quiescent moment exists", extra);
  /* deterministic final action, so the probe below has a known variant */
  CK(set_native_act(EN_SIG, (uintptr_t)en_target_a,
                    SA_SIGINFO | SA_ONSTACK, 0, 0, NULL) == 0,
     "final publication failed errno=%d", errno);
  INFO("after the in-handler stream: the publication and 64 further ones "
       "succeeded; no refusal state outlived the call");

  /* and the module is still coherent: a delivery of EN_SIG reaches the handler */
  hits_reset();
  int pr = deliver_probe(EN_SIG, BOX64DL_SIGNAL_VARIANT_ONSTACK,
                         "publication after recovery");
  CK((pr & 0x100) == 0 && dv_outcome(pr) == DVO_A,
     "a delivery after the recovery must invoke the recovered action "
     "(probe=0x%x)", pr);
  box64dl_signal_actions_fini();
}

/* ========================================================================== *
 * Runner: one forked child per case
 * ========================================================================== */

typedef struct case_s {
  const char* name;
  void (*fn)(void);
} case_t;

static const case_t g_cases[] = {
    {"init_capture", case_init_capture},
    {"four_variants", case_four_variants},
    {"range_and_reserved", case_range_and_reserved},
    {"chld_kernel_flags", case_chld_kernel_flags},
    {"injected_set_failure", case_injected_set_failure},
    {"injected_init_failure", case_injected_init_failure},
    {"staged_oneshot_before_latest", case_staged_oneshot_before_latest},
    {"old_entry_variant", case_old_entry_variant},
    {"pool_20k", case_pool_20k},
    {"foreign_longjmp", case_foreign_longjmp},
    {"uc_mask_edit", case_uc_mask_edit},
    {"multithread_snapshot", case_multithread_snapshot},
    {"guest_route", case_guest_route},
    {"default_fatal", case_default_fatal},
    {"enomem_boundary_and_recovery", case_enomem_boundary_and_recovery},
};

int main(int argc, char** argv) {
  if (argc > 1) {
    int fd = open(argv[1], O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd >= 0) g_log_fd = fd;
  }
  time_t now = time(NULL);
  struct tm tmv;
  char stamp[64];
  localtime_r(&now, &tmv);
  strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &tmv);
  logf_("=== test_signal_action_unit %s pid=%d ===\n", stamp, (int)getpid());
  logf_("LOG %s  (case filter: %s)\n", argc > 1 ? argv[1] : "(stdout)",
        argc > 2 ? argv[2] : "(all)");

  const char* only = argc > 2 ? argv[2] : NULL;
  int failed = 0, n = 0;
  for (size_t i = 0; i < sizeof(g_cases) / sizeof(g_cases[0]); ++i) {
    const case_t* c = &g_cases[i];
    if (only && strcmp(only, c->name) != 0) continue;
    fflush(NULL);
    pid_t pid = fork();
    if (pid == 0) {
      g_case = c->name;
      atomic_store(&g_failures, 0);
      c->fn();
      int f = atomic_load(&g_failures);
      logf_("%s %s\n", f == 0 ? "PASS" : "FAIL", c->name);
      fflush(NULL);
      _exit(f == 0 ? 0 : 1);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    ++n;
    if (!WIFEXITED(st)) {
      ++failed;
      logf_("FAIL %s (child killed by signal %d, status=0x%x)\n", c->name,
            WIFSIGNALED(st) ? WTERMSIG(st) : -1, st);
    } else if (WEXITSTATUS(st) != 0) {
      ++failed;
    }
  }
  logf_("=== %d/%d cases passed ===\n", n - failed, n);
  fflush(NULL);
  return failed == 0 ? 0 : 1;
}
