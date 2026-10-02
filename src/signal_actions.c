/*
 * libbox64dl: the process-wide application signal action set.
 *
 * See include/libbox64_signal_actions.h for the contract this file implements.
 * This translation unit deliberately includes NO engine header: it talks to the
 * engine only through the two symbols that header declares
 * (box64dl_engine_signal_hook, box64dl_signal_guest_deliver), so it compiles and
 * is unit-testable standalone and the engine's x64emu_t stays opaque.
 *
 * OBJECT GRAPH
 * -----------
 *   pairs[i] = { cur, reset }        one allocation unit, recycled as a unit
 *   slots[sig].latest                authoritative current node
 *   slots[sig].variant[v]            fast path for kernel entry variant v
 *   slots[sig].installed/…           the kernel disposition we installed
 *
 * `cur` and `reset` of a pair are the *only* two nodes of that pair, and the
 * pair is the unit of allocation and of retirement: a `reset` node is never
 * recycled on its own, so a recycled `cur` always has its preallocated one-shot
 * successor armed. A node is immutable once published: it is filled in while it
 * is still off every slot (it is on the freelist), then published with a store.
 * A publication never mutates a node that is reachable from a slot.
 *
 * READER GRACE PERIOD (sequential-consistency proof)
 * --------------------------------------------------
 * Reader:        A: fetch_add(readers, 1, seq_cst)
 *                S: seq_cst loads of the slot pointers it will use
 *                B: fetch_sub(readers, 1, seq_cst)
 * Writer:        W: seq_cst clearing stores to every slot rooting the node
 *                F: atomic_thread_fence(seq_cst)
 *                L: load(readers, seq_cst)
 *
 * If L returns 0 then no reader is between A and B, so no reader can still hold
 * a node retired by W: a reader that read a node at S read it before the
 * seq_cst store that removed it from that slot (an SC load reads the last
 * preceding store in the SC total order, and SC order follows program order),
 * hence S < W < F < L. If that reader's B were after L, then at L the counter
 * would still count it (A < S < L < B, and SC RMWs modify the counter in the SC
 * order), contradicting L == 0. So B < L as well, and the reader's entire use of
 * the node — which lies between A and B, because reader_enter()/reader_exit()
 * bracket it exactly — completes before the writer frees anything. Relaxed
 * fences (the previous shape) do not establish this; the counter RMWs, the slot
 * loads and the slot-clearing stores are therefore all seq_cst.
 *
 * reader_exit() always runs BEFORE any application callback, so a native or
 * guest handler that leaves its signal frame by a foreign longjmp cannot strand
 * a reference: the increment it owns is already balanced. This is why the module
 * needs no per-thread handler-depth latch and no refusal state that could
 * outlive a call.
 *
 * Balancing the reference is necessary but NOT sufficient on its own: the window
 * is still interruptible, and a handler that longjmps out of a reference-taking
 * frame (the getter, or a delivery that has not yet reached its callback) would
 * abandon the increment and permanently disable reclamation. Every reader window
 * is therefore additionally BOUNDED by a temporary deferral of all catchable
 * signals — see "Bounded reader window" below — so no longjmp can occur between
 * reader_enter() and reader_exit().
 *
 * MEMORY
 * ------
 * Every node comes from the preallocated pool and is recycled through a
 * freelist: nothing on the delivery path allocates or frees. The pool is the
 * bound on how many replacements can happen without a quiescent moment in
 * between (every set attempts a grace-period reclaim first). Exhaustion is
 * reported as ENOMEM, never as a silent drop.
 *
 * PUBLICATION ORDER
 * -----------------
 * A set plans the kernel action, publishes the TARGET variant slot, then
 * installs the kernel disposition, and publishes the authoritative `latest`
 * only after the install succeeded. A delivery that lands on the new kernel
 * entry therefore always finds a snapshot in the matching variant slot whose
 * logical flags produced that very entry, and a failed install leaves `latest`
 * untouched — no rollback of the authoritative action is needed, only of the
 * variant slot this set itself wrote. For a replacement whose entry and kernel
 * flags are unchanged the kernel is not MODIFIED; the plan is still validated
 * against the real disposition with a read-only sigaction() query, because the
 * kernel can have moved to SIG_DFL behind the module (the default-stop path) and
 * a stale cache must not make a needed re-install be skipped. The `latest`
 * exchange is the linearization point.
 *
 * VARIANT ROOTS ARE PER VARIANT, NOT PER SIGNAL
 * ---------------------------------------------
 * slots[s].variant[v] only ever holds a node whose variant_of_flags(flags) == v,
 * and a store into variant[v] never clears variant[w] for w != v. More than one
 * entry can be live for one signal — the installed one, plus an entry a delivery
 * may already be inside — so clearing a sibling root would both break the
 * agreement between an entry and the snapshot it loads and drop the last root of
 * a node that is still in use. A slot left holding a node that is no longer
 * `latest` is harmless: such a node is never recycled while it is rooted, and
 * take_action cross-checks the loaded node against the entry's variant before
 * delivering it.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <ucontext.h>
#include <unistd.h>

#include "libbox64_signal_actions.h"

/* ------------------------------------------------------------------------ *
 * Compile-time checks
 * ------------------------------------------------------------------------ */

_Static_assert(offsetof(box64_signal_action_t, kind) == 0,
               "action kind must be first");
_Static_assert(offsetof(box64_signal_action_t, flags) + 8 ==
                   offsetof(box64_signal_action_t, generation),
               "flags/generation are adjacent 64-bit fields");
_Static_assert(BOX64DL_SIGNAL_VARIANTS == 4, "four kernel entry variants");
_Static_assert(BOX64_SIGNAL_ACTION_DEFAULT == 0 && BOX64_SIGNAL_ACTION_GUEST == 3,
               "kind numbering is frozen");

/* ------------------------------------------------------------------------ *
 * State
 * ------------------------------------------------------------------------ */

typedef struct node_pair_s node_pair_t;

typedef struct action_node_s {
  box64dl_signal_action_t act; /* immutable once published */
  node_pair_t* pair;           /* the owning allocation unit */
  struct action_node_s* reset; /* this pair's one-shot successor */
  struct action_node_s* link;  /* freelist / retired threading (via pair->cur) */
  /* SA_RESETHAND consumption, per NODE and not per slot: this is what makes the
   * one-shot fire exactly once per generation even when `latest` has already
   * moved on to a newer replacement, and even when several deliveries read the
   * same node at once. Cleared only when the pair is recycled. */
  _Atomic int consumed;
} action_node_t;

/* One allocation unit: the current node and its preallocated one-shot reset
 * node. Both are recycled together, so a one-shot successor is always available
 * and nothing is ever allocated on a delivery path. */
struct node_pair_s {
  action_node_t cur;
  action_node_t reset;
  int state; /* 0 = free, 1 = live, 2 = retired */
};

typedef struct sig_slot_s {
  _Atomic(action_node_t*) latest; /* authoritative */
  _Atomic(action_node_t*) variant[BOX64DL_SIGNAL_VARIANTS];
  /* Generation is a per-signal lock-free reservation counter, not something
   * derived from a neighbour node. Deriving it (old->generation + 1) breaks as
   * soon as a one-shot consumption publishes its reset concurrently: the reset
   * and the replacement would then claim the same number. `fetch_add` gives
   * every publication — replacement or one-shot reset — a strictly newer unique
   * number, and it is a lock-free RMW, so the delivery path may use it. */
  _Atomic uint64_t gen_ctr;
  struct sigaction pre_kernel; /* exact pre-engine kernel pre-image */
  int have_pre_kernel;
  uint64_t gen_at_init; /* generation of the node init published */
  int installed;        /* 1: our dispatcher is the kernel disposition */
  int installed_variant;
  uint32_t installed_kflags; /* the full kernel sa_flags we installed */
} sig_slot_t;

static sig_slot_t g_slots[BOX64DL_SIGNAL_MAX + 1];

/* The pool is a FIXED bound, not an unbounded allocator: 512 pairs, all
 * preallocated at init, none ever allocated on a delivery path.
 *
 * What the size is chosen to cover is the number of SIMULTANEOUSLY ROOTED nodes,
 * not the number of writes. A node stays allocated while any slot roots it (or a
 * reader is inside its grace section), so the steady-state requirement is: every
 * tracked signal's capture node, plus, per signal, the nodes rooted by the four
 * variant slots and by `latest` — four nodes per tracked signal plus the
 * captures.
 *
 * It is explicitly NOT a promise that an unbounded stream of replacements
 * succeeds while readers keep the grace counter permanently non-zero:
 * reclaim_retired() only recycles a retired pair once it observes readers == 0
 * AND neither of the pair's nodes is still rooted, which is what makes reclamation
 * safe. A writer stream that never sees a quiescent moment therefore drains the
 * pool and is refused with ENOMEM, exactly as the header's ERRNO CONTRACT says.
 * That refusal is a clean, documented failure — never a silent drop and never an
 * unsafe free. */
#ifndef BOX64DL_NODE_PAIRS
#define BOX64DL_NODE_PAIRS 512
#endif
static node_pair_t g_pairs[BOX64DL_NODE_PAIRS];
static action_node_t* g_freelist; /* writer-private (under g_write_mutex) */
static action_node_t* g_retired;  /* writer-private */

static pthread_mutex_t g_write_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Reader grace counter; see READER GRACE PERIOD above. */
static _Atomic unsigned long g_readers;

static _Atomic int g_active;

/* Engine seam, defined by the engine (patches/0020 / 0021). Declared in the
 * header; repeated here so the standalone unit build links its own stubs. */
extern box64dl_engine_signal_hook_t box64dl_engine_signal_hook;

/* Optional NATIVE-invocation seam; see the header. Defined HERE, NULL by
 * default, so the standalone unit build needs no third-unit stub and every
 * existing caller keeps the direct-call behaviour. */
box64dl_native_call_hook_t box64dl_native_call_hook = NULL;

/* ------------------------------------------------------------------------ *
 * Reader grace period
 * ------------------------------------------------------------------------ */

static inline void reader_enter(void) {
  atomic_fetch_add_explicit(&g_readers, 1ul, memory_order_seq_cst);
}

static inline void reader_exit(void) {
  atomic_fetch_sub_explicit(&g_readers, 1ul, memory_order_seq_cst);
}

/* ------------------------------------------------------------------------ *
 * Bounded reader window
 * ------------------------------------------------------------------------ */

/* A reader reference must not be held across a call to an unknown application
 * handler, because such a handler may leave its frame by a foreign longjmp. The
 * DELIVERY path already satisfies that by releasing its reference before the
 * callback — but the window itself is still interruptible: a signal delivered
 * inside reader_enter()..reader_exit() whose handler longjmps would abandon the
 * reference, leaving g_readers permanently > 0. reclaim_retired() then bails out
 * forever, the fixed pool drains, and later setters are refused with ENOMEM even
 * though no thread is inside a reader section any more.
 *
 * Every reader window is therefore BOUNDED by temporarily deferring all catchable
 * signals for the few instructions that take and release the reference, then
 * restoring the caller's mask. On the delivery path the restore happens after
 * reader_exit() and before the application callback, so a signal deferred inside
 * the window (or any other pending signal) is delivered only once this thread no
 * longer holds a reference — deferred, never lost.
 *
 * This is not a userspace lock: it never waits on another thread and never
 * blocks; it only defers signal delivery to this thread for a bounded interval.
 * SIGKILL and SIGSTOP cannot be blocked and cannot longjmp out of this thread.
 * The set is built by libc via sigfillset() and applied with the POSIX
 * async-signal-safe pthread_sigmask(); no raw kernel sigaction/sigprocmask is
 * issued by hand, so the C library's internal 32/33 signals are never rewritten
 * here. */

static inline int reader_window_enter(sigset_t* saved) {
  sigset_t all;
  sigfillset(&all);
  return pthread_sigmask(SIG_SETMASK, &all, saved);
}

static inline int reader_window_exit(const sigset_t* saved) {
  return pthread_sigmask(SIG_SETMASK, saved, NULL);
}

/* 1 when the temporary-window primitives work on this target. Probed once at
 * init, so a well-formed process can never be forced into an unbounded window on
 * the delivery path. */
static int reader_window_usable(void) {
  sigset_t saved;
  if (reader_window_enter(&saved) != 0) return 0;
  return reader_window_exit(&saved) == 0;
}

/* ------------------------------------------------------------------------ *
 * Small helpers
 * ------------------------------------------------------------------------ */

/* Reason a signal cannot be tracked, as an errno value (0 = tracked). Signals 32
 * and 33 are the C library's internal pthread signals: see the header. */
static inline int sig_reject(int host_sig) {
  if (host_sig < BOX64DL_SIGNAL_MIN || host_sig > BOX64DL_SIGNAL_MAX)
    return EINVAL;
  if (host_sig == 32 || host_sig == 33) return ENOTSUP;
  return 0;
}

static inline int is_engine_fatal(int host_sig) {
  return host_sig == SIGSEGV || host_sig == SIGBUS || host_sig == SIGILL ||
         host_sig == SIGABRT;
}

/* The four kernel entries are keyed by the two logical flags, so the variant is
 * exactly the 2-bit encoding of (SA_ONSTACK, SA_RESTART). */
static inline int variant_of_flags(uint64_t flags) {
  uint32_t f = (uint32_t)flags;
  int v = 0;
  if (f & SA_ONSTACK) v |= 1;
  if (f & SA_RESTART) v |= 2;
  return v;
}

static uintptr_t handler_of_sigaction(const struct sigaction* sa) {
  if ((uint32_t)sa->sa_flags & SA_SIGINFO)
    return (uintptr_t)sa->sa_sigaction;
  return (uintptr_t)sa->sa_handler;
}

static int kind_of_handler(uintptr_t h) {
  if (h == (uintptr_t)SIG_DFL) return BOX64_SIGNAL_ACTION_DEFAULT;
  if (h == (uintptr_t)SIG_IGN) return BOX64_SIGNAL_ACTION_IGNORE;
  return BOX64_SIGNAL_ACTION_NATIVE;
}

static void fill_from_sigaction(box64dl_signal_action_t* out,
                                const struct sigaction* sa) {
  memset(out, 0, sizeof(*out));
  out->kind = kind_of_handler(handler_of_sigaction(sa));
  out->handler = (out->kind == BOX64_SIGNAL_ACTION_NATIVE)
                     ? handler_of_sigaction(sa)
                     : 0;
  out->restorer = (uintptr_t)sa->sa_restorer;
  out->mask = sa->sa_mask;
  /* Unsigned 32-bit widening: SA_RESETHAND is 0x80000000 and would otherwise
   * sign-extend through int -> uint64_t. */
  out->flags = (uint64_t)(uint32_t)sa->sa_flags;
  out->generation = 0;
}

/* ------------------------------------------------------------------------ *
 * The four static kernel entries
 * ------------------------------------------------------------------------ */

static void box64dl_dispatch_entry(int host_sig, siginfo_t* info, void* ucntx,
                                   int variant);

#define BOX64DL_DEFINE_ENTRY(NAME, VARIANT)                     \
  static void NAME(int32_t sig, siginfo_t* info, void* ucntx) { \
    box64dl_dispatch_entry((int)sig, info, ucntx, (VARIANT));   \
  }

BOX64DL_DEFINE_ENTRY(box64dl_entry_plain, 0)
BOX64DL_DEFINE_ENTRY(box64dl_entry_onstack, BOX64DL_SIGNAL_VARIANT_ONSTACK)
BOX64DL_DEFINE_ENTRY(box64dl_entry_restart, BOX64DL_SIGNAL_VARIANT_RESTART)
BOX64DL_DEFINE_ENTRY(box64dl_entry_mask, BOX64DL_SIGNAL_VARIANT_MASK)
#undef BOX64DL_DEFINE_ENTRY

static void (*const g_entries[BOX64DL_SIGNAL_VARIANTS])(int32_t, siginfo_t*,
                                                        void*) = {
    box64dl_entry_plain,
    box64dl_entry_onstack,
    box64dl_entry_restart,
    box64dl_entry_mask,
};

/* ------------------------------------------------------------------------ *
 * Kernel disposition
 * ------------------------------------------------------------------------ */

static void build_kernel_action(const box64dl_signal_action_t* a, int sig,
                                struct sigaction* out) {
  uint32_t f = (uint32_t)a->flags;
  memset(out, 0, sizeof(*out));
  /* An EMPTY sa_mask: the kernel adds only its own automatic self-block, and the
   * application's mask is applied by apply_app_mask() on the snapshot. */
  sigemptyset(&out->sa_mask);
  out->sa_sigaction = g_entries[variant_of_flags(a->flags)];
  uint32_t kf = (uint32_t)SA_SIGINFO;
  if (f & SA_ONSTACK) kf |= (uint32_t)SA_ONSTACK;
  if (f & SA_RESTART) kf |= (uint32_t)SA_RESTART;
  if (sig == SIGCHLD) {
    /* SIGCHLD's NOCLDWAIT/NOCLDSTOP kernel semantics belong to the application
     * even while our dispatcher owns the handler: propagate them. */
    if (f & SA_NOCLDWAIT) kf |= (uint32_t)SA_NOCLDWAIT;
    if (f & SA_NOCLDSTOP) kf |= (uint32_t)SA_NOCLDSTOP;
  }
  out->sa_flags = (int)kf;
  /* No SA_NODEFER and no SA_RESTHAND: the self-block and the one-shot reset are
   * implemented here on the immutable snapshot, so a concurrent replacement can
   * never be erased by a kernel reset. */
}

/* The application's action as a real kernel disposition: what the kernel should
 * hold whenever our dispatcher is not needed (DEFAULT/IGNORE on a non-fatal
 * signal), and what fini restores for an action replaced after init. The
 * application's flags are preserved — in particular SIGCHLD's
 * SA_NOCLDWAIT/SA_NOCLDSTOP, which are kernel semantics that outlive the
 * handler choice. SA_SIGINFO is dropped for a DFL/IGN disposition, which cannot
 * name a SIGINFO handler. */
static void build_true_kernel_action(const box64dl_signal_action_t* a,
                                     struct sigaction* out) {
  uint32_t f = (uint32_t)a->flags;
  memset(out, 0, sizeof(*out));
  if (a->kind == BOX64_SIGNAL_ACTION_DEFAULT) {
    out->sa_handler = SIG_DFL;
    out->sa_mask = a->mask;
    out->sa_flags = (int)(f & ~(uint32_t)SA_SIGINFO);
    return;
  }
  if (a->kind == BOX64_SIGNAL_ACTION_IGNORE) {
    out->sa_handler = SIG_IGN;
    out->sa_mask = a->mask;
    out->sa_flags = (int)(f & ~(uint32_t)SA_SIGINFO);
    return;
  }
  out->sa_mask = a->mask;
  out->sa_flags = (int)f;
  if (f & SA_SIGINFO)
    out->sa_sigaction = (void (*)(int, siginfo_t*, void*))a->handler;
  else
    out->sa_handler = (void (*)(int))a->handler;
  /* sa_restorer is recorded only; a guest address is never installed as a
   * kernel restorer. */
}

/* What the kernel disposition for `a` should be. */
typedef struct kernel_plan_s {
  struct sigaction sa;
  uint32_t kflags;
  int installed; /* 1: our dispatcher (always, for the four fatal signals) */
  int variant;   /* which entry, valid when installed */
} kernel_plan_t;

static void plan_kernel(const box64dl_signal_action_t* a, int sig,
                        kernel_plan_t* kp) {
  memset(kp, 0, sizeof(*kp));
  if (is_engine_fatal(sig) || a->kind == BOX64_SIGNAL_ACTION_NATIVE ||
      a->kind == BOX64_SIGNAL_ACTION_GUEST) {
    build_kernel_action(a, sig, &kp->sa);
    kp->installed = 1;
    kp->variant = variant_of_flags(a->flags);
  } else {
    build_true_kernel_action(a, &kp->sa);
    kp->installed = 0;
    kp->variant = variant_of_flags(a->flags);
  }
  kp->kflags = (uint32_t)kp->sa.sa_flags;
}

/* 1 when the kernel already holds exactly what `kp` asks for. The full kernel
 * sa_flags are compared, so a change that only moves SIGCHLD's
 * NOCLDWAIT/NOCLDSTOP (which does not change the entry variant) still forces a
 * kernel update. A true DFL/IGN disposition is re-applied unconditionally: it is
 * idempotent, and re-applying keeps "already correct" from depending on state we
 * would otherwise have to track exactly.
 *
 * The cached installed_* record is only a SHORTCUT, never the authority: the real
 * kernel disposition can change behind this module. The module's own
 * default-stop path (default_disposition() for SIGTSTP/SIGTTIN/SIGTTOU) raises
 * the signal with SIG_DFL and leaves the kernel at SIG_DFL. If "current" were
 * answered from the cache alone, a later controlled set planning the same variant
 * would skip the re-install and leave the kernel at SIG_DFL while reporting the
 * new logical action — silently losing the handler. So the real disposition is
 * queried here. This runs on the WRITER path only, under g_write_mutex, and
 * touches no writer-private field from a signal path. */
static int kernel_current(const sig_slot_t* slot, int host_sig,
                          const kernel_plan_t* kp) {
  if (!kp->installed) return 0;
  if (slot->installed != 1) return 0;
  if (slot->installed_variant != kp->variant ||
      slot->installed_kflags != kp->kflags)
    return 0;
  struct sigaction cur;
  memset(&cur, 0, sizeof(cur));
  if (sigaction(host_sig, NULL, &cur) != 0) return 0;
  if (!((uint32_t)cur.sa_flags & SA_SIGINFO)) return 0;
  if (cur.sa_sigaction != g_entries[kp->variant]) return 0;
  return (uint32_t)cur.sa_flags == (uint32_t)kp->kflags;
}

/* ------------------------------------------------------------------------ *
 * Pair pool
 * ------------------------------------------------------------------------ */

static void pair_push_free(node_pair_t* p) {
  p->state = 0;
  p->cur.link = g_freelist;
  g_freelist = &p->cur;
}

static void pool_init(void) {
  g_freelist = NULL;
  g_retired = NULL;
  for (int i = BOX64DL_NODE_PAIRS - 1; i >= 0; --i) {
    node_pair_t* p = &g_pairs[i];
    p->cur.pair = p;
    p->cur.reset = &p->reset;
    p->reset.pair = p;
    p->reset.reset = NULL;
    p->cur.link = NULL;
    p->reset.link = NULL;
    atomic_store_explicit(&p->cur.consumed, 0, memory_order_relaxed);
    atomic_store_explicit(&p->reset.consumed, 0, memory_order_relaxed);
    pair_push_free(p);
  }
}

/* Pop one whole pair and hand back its `cur` node, with `reset` armed as its
 * one-shot successor. The pair is the unit, so a `reset` node can never be
 * handed out as a `cur` (which would leave it without a successor and silently
 * break SA_RESETHAND). */
static action_node_t* freelist_pop(void) {
  action_node_t* n = g_freelist;
  if (!n) return NULL;
  g_freelist = n->link;
  node_pair_t* p = n->pair;
  p->state = 1;
  p->cur.link = NULL;
  p->reset.link = NULL;
  p->cur.reset = &p->reset;
  p->reset.reset = NULL;
  atomic_store_explicit(&p->cur.consumed, 0, memory_order_relaxed);
  atomic_store_explicit(&p->reset.consumed, 0, memory_order_relaxed);
  return &p->cur;
}

/* Arm `node`'s preallocated one-shot successor. SA_RESETHAND's kernel contract
 * is "the disposition becomes SIG_DFL" and the flag is cleared, so the successor
 * is a clean DEFAULT. Its generation is reserved from the same per-signal
 * counter as every other publication, so it is strictly newer than the node it
 * replaces and never collides with a concurrent replacement's number. */
static void arm_reset(action_node_t* node, uint64_t gen) {
  if (!node || !node->reset) return;
  box64dl_signal_action_t* r = &node->reset->act;
  memset(r, 0, sizeof(*r));
  r->kind = BOX64_SIGNAL_ACTION_DEFAULT;
  r->generation = gen;
}

/* 1 when any slot still roots `n`, so a reader could still load it. seq_cst
 * loads: this participates in the grace proof above. */
static int node_rooted(const action_node_t* n) {
  if (!n) return 0;
  for (int s = BOX64DL_SIGNAL_MIN; s <= BOX64DL_SIGNAL_MAX; ++s) {
    const sig_slot_t* slot = &g_slots[s];
    if (atomic_load_explicit(&slot->latest, memory_order_seq_cst) == n) return 1;
    for (int v = 0; v < BOX64DL_SIGNAL_VARIANTS; ++v)
      if (atomic_load_explicit(&slot->variant[v], memory_order_seq_cst) == n)
        return 1;
  }
  return 0;
}

/* Retire the pair owning `n`. The pair stays allocated until a later writer sees
 * the grace period quiescent AND neither of its two nodes is still rooted. A
 * `reset` node is retired with its `cur`, never on its own. */
static void retire_node(action_node_t* n) {
  if (!n || !n->pair) return;
  node_pair_t* p = n->pair;
  if (p->state == 2) return; /* already retired */
  p->state = 2;
  p->cur.link = g_retired;
  g_retired = &p->cur;
}

/* Recycle every retired pair no reader and no slot can still reach. Called only
 * by a writer holding g_write_mutex, with every catchable signal blocked on this
 * thread. */
static void reclaim_retired(void) {
  atomic_thread_fence(memory_order_seq_cst);
  if (atomic_load_explicit(&g_readers, memory_order_seq_cst) != 0) return;
  action_node_t* n = g_retired;
  g_retired = NULL;
  while (n) {
    action_node_t* next = n->link;
    node_pair_t* p = n->pair;
    if (!node_rooted(&p->cur) && !node_rooted(&p->reset)) {
      pair_push_free(p);
    } else {
      p->cur.link = g_retired;
      g_retired = &p->cur;
    }
    n = next;
  }
}

/* ------------------------------------------------------------------------ *
 * Getter
 * ------------------------------------------------------------------------ */

int box64dl_signal_action_get(int host_sig, box64dl_signal_action_t* out) {
  int bad = sig_reject(host_sig);
  if (bad) {
    errno = bad;
    return -1;
  }
  if (!atomic_load_explicit(&g_active, memory_order_acquire)) {
    errno = ENOSYS;
    return -1;
  }
  sig_slot_t* slot = &g_slots[host_sig];
  int entry_errno = errno;
  /* Bound the window: an interruptible reader section can be abandoned by a
   * handler that longjmps, which would strand the reference and permanently
   * disable reclamation. If the mask cannot be set we do NOT fall back to an
   * unbounded (unmasked) section: the caller gets a clear refusal instead. The
   * caller's mask and errno are both restored exactly. */
  sigset_t saved;
  if (reader_window_enter(&saved) != 0) {
    errno = EPERM;
    return -1;
  }
  reader_enter();
  action_node_t* n = atomic_load_explicit(&slot->latest, memory_order_seq_cst);
  if (out) {
    if (n) {
      *out = n->act; /* plain copy of immutable fields */
    } else {
      memset(out, 0, sizeof(*out));
      out->kind = BOX64_SIGNAL_ACTION_DEFAULT;
    }
  }
  reader_exit();
  if (reader_window_exit(&saved) != 0) {
    errno = EPERM;
    return -1;
  }
  errno = entry_errno; /* success leaves errno exactly as found */
  return 0;
}

/* ------------------------------------------------------------------------ *
 * Setter
 * ------------------------------------------------------------------------ */

static int set_locked(int host_sig, const box64dl_signal_action_t* act,
                      box64dl_signal_action_t* old) {
  sig_slot_t* slot = &g_slots[host_sig];

  if (!act) { /* query only */
    action_node_t* oldnode =
        atomic_load_explicit(&slot->latest, memory_order_seq_cst);
    if (old) {
      if (oldnode)
        *old = oldnode->act;
      else {
        memset(old, 0, sizeof(*old));
        old->kind = BOX64_SIGNAL_ACTION_DEFAULT;
      }
    }
    return 0;
  }

  reclaim_retired();
  action_node_t* node = freelist_pop();
  if (!node) {
    errno = ENOMEM;
    return -1;
  }
  node->act = *act;
  node->act._reserved0 = 0;
  /* The counter is the single source of generation numbers: reserve ours now,
   * and reserve the successor's eagerly so the one-shot path (which must not
   * allocate and should not touch the counter twice) has a fresh number ready
   * that no concurrent writer can claim. */
  node->act.generation =
      atomic_fetch_add_explicit(&slot->gen_ctr, 1, memory_order_relaxed) + 1;
  arm_reset(node, atomic_fetch_add_explicit(&slot->gen_ctr, 1,
                                            memory_order_relaxed) + 1);

  kernel_plan_t kp;
  plan_kernel(&node->act, host_sig, &kp);
  int v = kp.variant;
  int need_install = !kernel_current(slot, host_sig, &kp);

  /* Publish the TARGET variant slot before touching the kernel, so a delivery
   * that lands on the new entry always finds a snapshot whose logical flags
   * produced that entry. The OTHER variant slots are left exactly as they are:
   * they root nodes of their own variant, and clearing one would strip a node a
   * delivery may still be reading. A stale sibling root is safe — take_action
   * cross-checks the node against the entry's variant, and a rooted node is
   * never recycled. */
  action_node_t* saved_target =
      atomic_load_explicit(&slot->variant[v], memory_order_seq_cst);
  atomic_store_explicit(&slot->variant[v], node, memory_order_seq_cst);

  if (need_install && sigaction(host_sig, &kp.sa, NULL) != 0) {
    int e = errno;
    /* The authoritative `latest` was never touched, so the previous action is
     * still fully in force. Undo exactly the one store this set published: put
     * the previous node of THIS variant back, and only if the slot still holds
     * the failed candidate — a concurrent consumption may already have cleared
     * it, in which case the newer value must win and is left alone.
     *
     * The candidate PAIR is then retired, never pushed straight back onto the
     * freelist: a concurrent delivery may have loaded the candidate from the
     * variant slot before the CAS above put the old node back, and it is still
     * inside its reader section. Retiring it makes the recycle conditional on
     * the grace period being empty and on neither node of the pair being rooted;
     * the immediate free was a live use-after-free. */
    action_node_t* expect = node;
    atomic_compare_exchange_strong_explicit(&slot->variant[v], &expect,
                                            saved_target,
                                            memory_order_seq_cst,
                                            memory_order_seq_cst);
    retire_node(node);
    /* `old` reports the action that is ACTUALLY still installed, not the one
     * that failed: the authoritative `latest` was never touched, so it still
     * names the pre-existing action. Reporting the failed candidate here would
     * be a lie about both the action and its generation (the failed candidate is
     * never published and its generation gap is not observable). */
    if (old) {
      action_node_t* cur =
          atomic_load_explicit(&slot->latest, memory_order_seq_cst);
      if (cur) {
        *old = cur->act;
      } else {
        memset(old, 0, sizeof(*old));
        old->kind = BOX64_SIGNAL_ACTION_DEFAULT;
      }
    }
    errno = e;
    return -1;
  }

  if (need_install) {
    slot->installed = kp.installed;
    slot->installed_variant = kp.installed ? kp.variant : -1;
    slot->installed_kflags = kp.installed ? kp.kflags : 0;
  }

  /* THE LINEARIZATION POINT. `exchange` hands back the node that was actually
   * current here, which is the only node this replacement truly replaced: a
   * concurrent one-shot consumption may have installed its reset between the
   * plan above and this store, and reporting the node we happened to read
   * earlier as "the replaced action" would be a lie about both the action and
   * its generation. */
  action_node_t* replaced =
      atomic_exchange_explicit(&slot->latest, node, memory_order_seq_cst);

  if (old && replaced) *old = replaced->act;

  /* Post-exchange reconciliation for a consumed node.
   *
   * A delivery that entered the variant slot this set published may have run,
   * copied the node and won its one-shot CAS BEFORE this exchange ran. In that
   * interleaving the consumption's own conditional CAS found `latest` still
   * naming the previous node, so it did not install the reset; without this
   * step the exchange above would publish an already-consumed node as the
   * authoritative action and the SA_RESETHAND reset would be lost — a later
   * query would report the old one-shot action as still current.
   *
   * The CAS is conditional on `latest` naming the node, so a newer registration
   * that landed after the exchange is never erased, and a reader that consumes
   * after this check performs the same CAS itself and wins. */
  if (atomic_load_explicit(&node->consumed, memory_order_seq_cst) == 1) {
    action_node_t* nexp = node;
    atomic_compare_exchange_strong_explicit(&slot->latest, &nexp, node->reset,
                                            memory_order_seq_cst,
                                            memory_order_seq_cst);
  }

  /* Retire by PAIR, never per node: the replaced node and the one-shot reset
   * that a concurrent consumption may also have displaced can belong to the same
   * allocation unit, and retiring that unit twice would corrupt the freelist.
   * The node this set displaced from the TARGET variant slot lost its last root
   * here as well; if it is not the node the exchange replaced (a stale sibling
   * variant of an older, already-superseded action) it must still be retired so
   * the pair can be recycled once no reader can reach it. */
  retire_node(replaced);
  if (saved_target && saved_target != replaced) retire_node(saved_target);
  return 0;
}

static int set_common(int host_sig, const box64dl_signal_action_t* act,
                      box64dl_signal_action_t* old) {
  int bad = sig_reject(host_sig);
  if (bad) {
    errno = bad;
    return -1;
  }
  if (!atomic_load_explicit(&g_active, memory_order_acquire)) {
    errno = ENOSYS;
    return -1;
  }
  if (act) {
    if (act->kind < BOX64_SIGNAL_ACTION_DEFAULT ||
        act->kind > BOX64_SIGNAL_ACTION_GUEST) {
      errno = EINVAL;
      return -1;
    }
    if (act->kind == BOX64_SIGNAL_ACTION_NATIVE &&
        (act->handler == 0 || act->handler == (uintptr_t)SIG_ERR)) {
      errno = EINVAL;
      return -1;
    }
    /* Linux rejects these two for rt_sigaction: no handler can ever be
     * installed, so recording one would be a lie. */
    if (host_sig == SIGKILL || host_sig == SIGSTOP) {
      errno = EINVAL;
      return -1;
    }
  }

  /* Serialize writers with every catchable signal blocked on this thread, so a
   * delivery cannot re-enter the writer through this same thread. */
  sigset_t all, saved;
  sigfillset(&all);
  pthread_sigmask(SIG_SETMASK, &all, &saved);

  int rc;
  if (pthread_mutex_trylock(&g_write_mutex) != 0) {
    /* Refusal happens before any state is touched. */
    pthread_sigmask(SIG_SETMASK, &saved, NULL);
    errno = EWOULDBLOCK;
    return -1;
  }
  rc = set_locked(host_sig, act, old);
  pthread_mutex_unlock(&g_write_mutex);
  pthread_sigmask(SIG_SETMASK, &saved, NULL);
  return rc;
}

int box64dl_signal_action_set(int host_sig, const box64dl_signal_action_t* act,
                              box64dl_signal_action_t* old) {
  return set_common(host_sig, act, old);
}

/* ------------------------------------------------------------------------ *
 * Public API
 * ------------------------------------------------------------------------ */

BOX64_API int box64_signal_action_get(int host_sig,
                                      box64_signal_action_t* out) {
  if (!out) {
    errno = EFAULT;
    return -1;
  }
  return box64dl_signal_action_get(host_sig, out);
}

BOX64_API int box64_signal_action_set_native(int host_sig,
                                             const struct sigaction* act,
                                             box64_signal_action_t* old) {
  if (!act) return box64dl_signal_action_set(host_sig, NULL, old);
  if (act->sa_handler == SIG_ERR) {
    errno = EINVAL;
    return -1;
  }
  box64dl_signal_action_t a;
  fill_from_sigaction(&a, act);
  if (a.kind == BOX64_SIGNAL_ACTION_NATIVE && a.handler == 0) {
    errno = EINVAL;
    return -1;
  }
  return box64dl_signal_action_set(host_sig, &a, old);
}

/* ------------------------------------------------------------------------ *
 * Delivery
 * ------------------------------------------------------------------------ */

/* Apply the application's logical mask for the duration of the handler: the
 * pre-delivery mask the kernel recorded in the ucontext, unioned with the
 * snapshot mask, plus the signal itself unless the application asked for
 * SA_NODEFER. The ucontext is left untouched, so the kernel's return path
 * restores exactly what the delivery recorded, including any change the handler
 * made. */
static void apply_app_mask(const box64dl_signal_action_t* a, int host_sig,
                           void* native_uc) {
  sigset_t want;
  if (native_uc) {
    want = ((ucontext_t*)native_uc)->uc_sigmask;
  } else if (sigprocmask(SIG_SETMASK, NULL, &want) != 0) {
    sigemptyset(&want);
  }
  for (int s = 1; s < 65; ++s)
    if (sigismember(&a->mask, s) == 1) sigaddset(&want, s);
  if (!((uint32_t)a->flags & SA_NODEFER)) sigaddset(&want, host_sig);
  sigprocmask(SIG_SETMASK, &want, NULL);
}

/* Defined below; the window failure path reuses the irreversible terminal
 * disposition for a fatal engine signal. */
static void __attribute__((noreturn)) terminal_default(int host_sig);

static void __attribute__((noreturn))
window_terminal_failure(const char* msg, size_t len, int host_sig);

/* The length is taken from the literal itself, so it can never drift from the
 * message (an explicit count is exactly the kind of thing that rots). */
#define WINDOW_FAIL(literal, host_sig) \
  window_terminal_failure((literal), sizeof(literal) - 1, (host_sig))

/* The bounded reader window could not be established or restored: the module
 * cannot make a delivery memory-safe, and a void entry point has no errno to
 * report it. There is no safe fallback — running the callback in an unguarded
 * reader section is exactly the stranding bug the window exists to prevent, and
 * silently dropping the delivery would lose the application's action — so this
 * is an explicit terminal failure: an async-signal-safe write(2) diagnostic (no
 * stdio, no allocator, no lock, no format machinery; the messages are literals),
 * then the irreversible terminal disposition. A fatal engine signal keeps its
 * real identity (SIG_DFL + re-raise on this thread, so core/exit status stay
 * faithful); every other signal exits with the conventional 127.
 *
 * Unreachable in a well-formed process: init() refuses startup with EPERM when
 * the window primitives do not work. It exists so that an invariant break is
 * loud instead of silently unsafe. */
static void __attribute__((noreturn))
window_terminal_failure(const char* msg, size_t len, int host_sig) {
  ssize_t ignored = write(STDERR_FILENO, msg, len);
  (void)ignored;
  if (is_engine_fatal(host_sig)) terminal_default(host_sig);
  _exit(127);
}

/* A fatal signal whose application action is the default cannot be resumed: the
 * engine has already run its repairs (the hook ran). Apply the irreversible
 * terminal default — install SIG_DFL, unblock self, re-raise on this very thread
 * — and never return into the delivery loop. */
static void __attribute__((noreturn)) terminal_default(int host_sig) {
  struct sigaction dfl;
  memset(&dfl, 0, sizeof(dfl));
  dfl.sa_handler = SIG_DFL;
  sigemptyset(&dfl.sa_mask);
  sigaction(host_sig, &dfl, NULL);

  sigset_t one;
  sigemptyset(&one);
  sigaddset(&one, host_sig);
  sigprocmask(SIG_UNBLOCK, &one, NULL);

  pid_t tid = (pid_t)syscall(SYS_gettid);
  pid_t pid = getpid();
  syscall(SYS_tgkill, pid, tid, host_sig);

  /* Defensive second attempt if the disposition change had not taken effect
   * yet, then the conventional signal exit status. */
  sigaction(host_sig, &dfl, NULL);
  sigprocmask(SIG_UNBLOCK, &one, NULL);
  syscall(SYS_tgkill, pid, tid, host_sig);
  _exit(128 + host_sig);
}

static void emit_default(box64dl_signal_action_t* out) {
  memset(out, 0, sizeof(*out));
  out->kind = BOX64_SIGNAL_ACTION_DEFAULT;
}

/* Copy the action for this delivery and consume a one-shot generation.
 *
 * Three things have to be exactly right:
 *
 *  1. WHICH snapshot is delivered. A delivery enters kernel entry `variant`, so
 *     it may only ever deliver a snapshot whose logical flags produced that very
 *     variant: variant_of_flags(snapshot.flags) == variant. The matching variant
 *     slot is the fast path. Falling back to `latest` is allowed ONLY while
 *     `latest` still agrees with the entry's variant; a `latest` that does not
 *     agree belongs to a newer kernel action that a later delivery would have
 *     taken, and delivering it here would give this frame the restart/altstack
 *     semantics of an entry the kernel did not use. When neither agrees the
 *     honest answer is DEFAULT — never a mismatched newer action.
 *
 *  2. HOW MANY TIMES a SA_RESETHAND action is invoked. That is a CAS on the NODE
 *     (`n->consumed`), not on the slot: it fires exactly once per node even if
 *     `latest` has already moved on, and even if several deliveries loaded the
 *     same node concurrently (exactly one wins the CAS).
 *
 *  3. WHAT A LOSER DELIVERS. A delivery that loses the consumption CAS must NOT
 *     dispatch whatever `latest` happens to hold: that value belongs to a newer
 *     registration which the winner's consumption may not have installed, and
 *     invoking a newer one-shot handler for an older captured entry is exactly
 *     how one handler fires twice. The loser delivers the consumed node's own
 *     reset outcome (logical DEFAULT), which is what this generation's one-shot
 *     contract reduced it to.
 *
 * All of it completes under the caller's reader reference, before that reference
 * is released and before any application callback. The pointer mutations below
 * are seq_cst: they are part of the grace proof in the file header, whose
 * ordering guarantee requires the clearing stores to be SC. */
static void take_action(int host_sig, int variant, box64dl_signal_action_t* out) {
  sig_slot_t* slot = &g_slots[host_sig];
  action_node_t* n = NULL;
  int have_variant = (variant >= 0 && variant < BOX64DL_SIGNAL_VARIANTS);
  if (have_variant)
    n = atomic_load_explicit(&slot->variant[variant], memory_order_seq_cst);
  /* The matching entry and the snapshot must agree. A node whose flags produce a
   * different variant was rooted by a different kernel action; it is not this
   * delivery's action. */
  if (n && variant_of_flags(n->act.flags) != variant) n = NULL;
  if (!n) {
    action_node_t* cand =
        atomic_load_explicit(&slot->latest, memory_order_seq_cst);
    /* Never substitute a snapshot whose variant disagrees with the entry that
     * delivered us. During the window between publishing the target variant slot
     * and exchanging `latest`, or between two installs, `latest` can legitimately
     * belong to another entry. */
    if (cand && (!have_variant || variant_of_flags(cand->act.flags) == variant))
      n = cand;
  }
  if (!n) {
    emit_default(out);
    return;
  }
  *out = n->act;

  if (!((uint32_t)out->flags & SA_RESETHAND) || !n->reset) return;

  int expected = 0;
  if (!atomic_compare_exchange_strong_explicit(&n->consumed, &expected, 1,
                                               memory_order_seq_cst,
                                               memory_order_seq_cst)) {
    /* Another delivery already consumed this node. Deliver THIS node's own
     * reset snapshot — the preallocated DEFAULT successor — never whatever newer
     * action `latest` now names. n->reset is non-NULL here (checked above) and is
     * immutable while n is rooted, so this is the exact disposition the one-shot
     * contract reduced this generation to, with the generation the winner
     * published; it also matches what a subsequent get() reports. */
    *out = n->reset->act;
    return;
  }

  /* Won the consumption: this delivery invokes the snapshot it copied, and the
   * preallocated reset becomes `latest` — but only if `latest` still holds this
   * very node, so a newer replacement is never erased. When `latest` does not
   * hold it yet (this delivery raced ahead of the setter that published the node
   * into the variant slot), the CAS simply does not install the reset; the setter
   * performs the equivalent reconciliation after its own exchange. */
  action_node_t* exp = n;
  atomic_compare_exchange_strong_explicit(&slot->latest, &exp, n->reset,
                                          memory_order_seq_cst,
                                          memory_order_seq_cst);
}

/* The default disposition of a fatal signal that is NOT one of the four engine
 * signals. Returning is the correct kernel-like behaviour for default-ignore and
 * for a stop signal; everything else is a real termination. This is the honest
 * mapping of the kernel's default table. */
static void default_disposition(int host_sig, void* native_uc) {
  if (is_engine_fatal(host_sig)) terminal_default(host_sig);
  if (host_sig == SIGCHLD || host_sig == SIGCONT || host_sig == SIGURG ||
      host_sig == SIGWINCH) {
    return; /* default ignore */
  }
  if (host_sig == SIGTSTP || host_sig == SIGTTIN || host_sig == SIGTTOU) {
    /* Default stop. Raise it with SIG_DFL and let the kernel stop us; on return
     * (SIGCONT) the original instruction/handler context continues. */
    struct sigaction dfl;
    memset(&dfl, 0, sizeof(dfl));
    dfl.sa_handler = SIG_DFL;
    sigemptyset(&dfl.sa_mask);
    sigaction(host_sig, &dfl, NULL);
    sigset_t one;
    sigemptyset(&one);
    sigaddset(&one, host_sig);
    sigprocmask(SIG_UNBLOCK, &one, NULL);
    syscall(SYS_tgkill, getpid(), (pid_t)syscall(SYS_gettid), host_sig);
    (void)native_uc;
    return;
  }
  terminal_default(host_sig);
}

static void invoke_action(const box64dl_signal_action_t* a, int host_sig,
                          siginfo_t* info, void* native_uc, void* dynablock,
                          uintptr_t x64pc, x64emu_t* emu) {
  switch (a->kind) {
    case BOX64_SIGNAL_ACTION_IGNORE:
      return;
    case BOX64_SIGNAL_ACTION_NATIVE: {
      int saved_errno = errno;
      /* The logical mask is applied HERE, uniformly, before the handler runs:
       * whether the invocation goes through the optional wrapper or directly,
       * the handler observes the same mask. The wrapper must not re-apply it. */
      apply_app_mask(a, host_sig, native_uc);
      box64dl_native_call_hook_t hook = box64dl_native_call_hook;
      if (hook) {
        /* Engine-provided wrapper: it owns the TLS suspend/restore needed when a
         * native handler is entered while a guest bridge frame is active, and it
         * invokes the handler named by `a` itself. The mask above is already
         * applied and errno is preserved below, so the wrapper does neither. */
        hook(a, host_sig, info, native_uc);
      } else if ((uint32_t)a->flags & SA_SIGINFO) {
        void (*fn)(int, siginfo_t*, void*) =
            (void (*)(int, siginfo_t*, void*))a->handler;
        fn(host_sig, info, native_uc);
      } else {
        void (*fn)(int) = (void (*)(int))a->handler;
        fn(host_sig);
      }
      errno = saved_errno;
      return;
    }
    case BOX64_SIGNAL_ACTION_GUEST:
      apply_app_mask(a, host_sig, native_uc);
      box64dl_signal_guest_deliver(emu, host_sig, info, native_uc, a, dynablock,
                                   x64pc);
      return;
    case BOX64_SIGNAL_ACTION_DEFAULT:
    default:
      default_disposition(host_sig, native_uc);
      return;
  }
}

void box64dl_signal_deliver(x64emu_t* emu, int host_sig, siginfo_t* info,
                            void* native_uc, int variant, void* dynablock,
                            uintptr_t x64pc) {
  if (sig_reject(host_sig)) return;

  box64dl_signal_action_t act;

  if (!atomic_load_explicit(&g_active, memory_order_acquire)) {
    /* Finalized (or never initialized) while our dispatcher was still the
     * kernel disposition: never return into a frame that would re-execute a
     * fatal fault forever. */
    if (is_engine_fatal(host_sig)) terminal_default(host_sig);
    return;
  }

  /* Bound the reader window, then release the reference and restore the mask
   * BEFORE the application callback: a signal deferred inside the window (or any
   * other pending signal) becomes deliverable again only after this thread no
   * longer holds a reference.
   *
   * A failure here is terminal, never a fallback: without the guard a handler
   * that longjmps out of the reader section would strand the reference and
   * permanently disable reclamation, and this void entry point cannot report that
   * as an errno. So the reference is not even taken on an enter failure, and an
   * unguarded callback or a silent drop is never chosen. */
  sigset_t saved;
  if (reader_window_enter(&saved) != 0)
    WINDOW_FAIL("libbox64dl: reader window enter failed\n", host_sig);
  reader_enter();
  take_action(host_sig, variant, &act);
  reader_exit();
  /* The reference is already released here, so this is not a leak; but the
   * thread would be left with every catchable signal blocked, which is not a
   * state the application's mask contract permits a handler to run in. */
  if (reader_window_exit(&saved) != 0)
    WINDOW_FAIL("libbox64dl: reader window restore failed\n", host_sig);

  invoke_action(&act, host_sig, info, native_uc, dynablock, x64pc, emu);
}

/* Entry point shared by the four static kernel entries. A user-generated signal
 * (si_code <= 0: SI_USER/SI_QUEUE/SI_TKILL/SI_TIMER/...) needs none of the
 * engine's fault maintenance and goes straight to the application decision. A
 * hardware fault on one of the four engine signals goes through the engine's own
 * repair hook, which calls box64dl_signal_deliver() when the repairs are done. */
static void box64dl_dispatch_entry(int host_sig, siginfo_t* info, void* ucntx,
                                   int variant) {
  if (is_engine_fatal(host_sig) && info && info->si_code > 0 &&
      box64dl_engine_signal_hook) {
    box64dl_engine_signal_hook((int32_t)host_sig, info, ucntx, variant);
    return;
  }
  box64dl_signal_deliver(NULL, host_sig, info, ucntx, variant, NULL,
                         (uintptr_t)-1);
}

/* ------------------------------------------------------------------------ *
 * Lifecycle
 * ------------------------------------------------------------------------ */

/* Install our dispatcher for `sig` from the plan, recording what the kernel now
 * holds. Returns 0, or -1 with errno from sigaction(). */
static int install_dispatcher(int sig, sig_slot_t* slot,
                             const kernel_plan_t* kp) {
  if (sigaction(sig, &kp->sa, NULL) != 0) return -1;
  slot->installed = 1;
  slot->installed_variant = kp->variant;
  slot->installed_kflags = kp->kflags;
  return 0;
}

/* Verify that every atomic word the module's proofs depend on is really
 * lock-free on this target. A fallback to a libatomic lock would break the
 * grace-period proof and make the getter unsafe from a signal handler, and the
 * delivery path would take a lock it must never take. Checked BEFORE anything is
 * installed, so a refusal leaves the process completely untouched. */
static int atomics_lock_free(void) {
  if (!atomic_is_lock_free(&g_readers)) return 0;
  if (!atomic_is_lock_free(&g_active)) return 0;
  for (int s = BOX64DL_SIGNAL_MIN; s <= BOX64DL_SIGNAL_MAX; ++s) {
    if (!atomic_is_lock_free(&g_slots[s].latest)) return 0;
    if (!atomic_is_lock_free(&g_slots[s].gen_ctr)) return 0;
    for (int v = 0; v < BOX64DL_SIGNAL_VARIANTS; ++v)
      if (!atomic_is_lock_free(&g_slots[s].variant[v])) return 0;
  }
  for (int i = 0; i < BOX64DL_NODE_PAIRS; ++i) {
    if (!atomic_is_lock_free(&g_pairs[i].cur.consumed)) return 0;
    if (!atomic_is_lock_free(&g_pairs[i].reset.consumed)) return 0;
  }
  return 1;
}

/* Restore every slot whose kernel disposition is ours to the captured pre-init
 * action and clear the record. This is init's rollback: a partial install is
 * never reported as success and never left behind. */
static void rollback_installs(void) {
  for (int s = BOX64DL_SIGNAL_MIN; s <= BOX64DL_SIGNAL_MAX; ++s) {
    if (sig_reject(s)) continue;
    sig_slot_t* slot = &g_slots[s];
    if (!slot->installed) continue;
    if (slot->have_pre_kernel) {
      sigaction(s, &slot->pre_kernel, NULL);
    } else {
      struct sigaction dfl;
      memset(&dfl, 0, sizeof(dfl));
      dfl.sa_handler = SIG_DFL;
      sigaction(s, &dfl, NULL);
    }
    slot->installed = 0;
    slot->installed_variant = -1;
    slot->installed_kflags = 0;
  }
}

int box64dl_signal_actions_init(void) {
  if (atomic_load_explicit(&g_active, memory_order_acquire)) return 0;
  if (!atomics_lock_free()) {
    errno = ENOTSUP;
    return -1;
  }
  /* The reader window must be boundable on this target, or every reader section
   * would be abandonable by a foreign longjmp. Refuse startup rather than run
   * with an unbounded window. */
  if (!reader_window_usable()) {
    errno = EPERM;
    return -1;
  }

  memset(g_slots, 0, sizeof(g_slots));
  pool_init();

  /* Phase 1: capture every valid application action as it is BEFORE the engine
   * installs anything. That capture is the "before engine init" snapshot and it
   * is what a later fini restores. Nothing has been installed yet, so a failure
   * here has nothing to roll back — but it is still fatal: fabricating a DEFAULT
   * for a capture that failed would report an action the application never had,
   * and a later fini would then overwrite the real disposition with that
   * fabrication. A raw kernel handler is never kept as a bypass either. */
  for (int s = BOX64DL_SIGNAL_MIN; s <= BOX64DL_SIGNAL_MAX; ++s) {
    if (sig_reject(s)) continue;
    sig_slot_t* slot = &g_slots[s];
    struct sigaction cur;
    memset(&cur, 0, sizeof(cur));
    if (sigaction(s, NULL, &cur) != 0) {
      errno = EPERM;
      return -1;
    }
    slot->pre_kernel = cur;
    slot->have_pre_kernel = 1;
    box64dl_signal_action_t cap;
    fill_from_sigaction(&cap, &slot->pre_kernel);
    action_node_t* n = freelist_pop();
    if (!n) {
      errno = ENOMEM;
      return -1;
    }
    /* Generation 1 is the first publication on this signal: a captured action
     * is a real generation, and starting the counter at 0 keeps the successors
     * reserved from the same sequence. */
    n->act = cap;
    n->act.generation =
        atomic_fetch_add_explicit(&slot->gen_ctr, 1, memory_order_relaxed) + 1;
    arm_reset(n, atomic_fetch_add_explicit(&slot->gen_ctr, 1,
                                           memory_order_relaxed) + 1);
    atomic_store_explicit(&slot->variant[variant_of_flags(n->act.flags)], n,
                          memory_order_seq_cst);
    atomic_store_explicit(&slot->latest, n, memory_order_seq_cst);
    slot->gen_at_init = n->act.generation;
    slot->installed = 0;
    slot->installed_variant = -1;
    slot->installed_kflags = 0;
  }

  /* Phase 2: the four engine signals are MANDATORY and transactional: install on
   * all four, or roll every partial install back to the exact pre-init action
   * and report failure. The module stays inactive, so every entry point reports
   * ENOSYS; a partial install is never reported as success.
   *
   * BOX64DL_TEST_FAIL_INSTALL_NTH exists so a unit test can inject a failure at
   * the Nth mandatory install (1-based) and observe the rollback; it has no
   * effect unless it is defined at compile time. */
  static const int mandatory[4] = {SIGSEGV, SIGBUS, SIGILL, SIGABRT};
  for (int i = 0; i < 4; ++i) {
    int s = mandatory[i];
    sig_slot_t* slot = &g_slots[s];
    action_node_t* n = atomic_load_explicit(&slot->latest, memory_order_seq_cst);
    kernel_plan_t kp;
    plan_kernel(&n->act, s, &kp);
    int injected = 0;
    int installed_ok = 0;
#ifdef BOX64DL_TEST_FAIL_INSTALL_NTH
    if (i + 1 == (BOX64DL_TEST_FAIL_INSTALL_NTH)) injected = 1;
#endif
    if (!injected) installed_ok = (install_dispatcher(s, slot, &kp) == 0);
    if (!installed_ok) {
      rollback_installs();
      errno = EPERM;
      return -1;
    }
  }

  /* Phase 3: every other tracked signal whose captured action is a NATIVE or
   * GUEST handler is virtualized too — otherwise a captured native action would
   * keep the kernel's own disposition and silently bypass the one-shot
   * consumption, the logical mask and the getter's view.
   *
   * There is NO graceful-degradation path for this: the getter reports the
   * logical snapshot and cannot report that the kernel is still running a raw
   * handler itself. So a failure here rolls the whole transaction back and init
   * fails, rather than reporting a coverage this module does not have. */
  for (int s = BOX64DL_SIGNAL_MIN; s <= BOX64DL_SIGNAL_MAX; ++s) {
    if (sig_reject(s) || is_engine_fatal(s)) continue;
    if (s == SIGKILL || s == SIGSTOP) continue;
    sig_slot_t* slot = &g_slots[s];
    action_node_t* n = atomic_load_explicit(&slot->latest, memory_order_seq_cst);
    if (n->act.kind != BOX64_SIGNAL_ACTION_NATIVE &&
        n->act.kind != BOX64_SIGNAL_ACTION_GUEST)
      continue;
    kernel_plan_t kp;
    plan_kernel(&n->act, s, &kp);
    if (install_dispatcher(s, slot, &kp) != 0) {
      rollback_installs();
      errno = EPERM;
      return -1;
    }
  }

  atomic_store_explicit(&g_active, 1, memory_order_release);
  return 0;
}

int box64dl_signal_actions_active(void) {
  return atomic_load_explicit(&g_active, memory_order_acquire) ? 1 : 0;
}

void box64dl_signal_actions_fini(void) {
  if (!atomic_load_explicit(&g_active, memory_order_acquire)) return;

  for (int s = BOX64DL_SIGNAL_MIN; s <= BOX64DL_SIGNAL_MAX; ++s) {
    if (sig_reject(s)) continue;
    sig_slot_t* slot = &g_slots[s];
    if (!slot->installed) continue;

    /* Only touch a disposition still ours: a host action installed after init is
     * left alone. */
    struct sigaction cur;
    if (sigaction(s, NULL, &cur) != 0) continue;
    int ours = 0;
    if ((uint32_t)cur.sa_flags & SA_SIGINFO) {
      for (int v = 0; v < BOX64DL_SIGNAL_VARIANTS; ++v)
        if (cur.sa_sigaction == g_entries[v]) ours = 1;
    }
    if (!ours) continue;

    action_node_t* n = atomic_load_explicit(&slot->latest, memory_order_seq_cst);
    if (n && n->act.generation != slot->gen_at_init) {
      /* The application replaced this action through the controlled API after
       * init: restore that live action as a real kernel disposition. A guest
       * address cannot outlive the engine, so a GUEST action falls back to the
       * captured pre-init action instead. */
      if (n->act.kind == BOX64_SIGNAL_ACTION_GUEST) {
        if (slot->have_pre_kernel) sigaction(s, &slot->pre_kernel, NULL);
        else {
          struct sigaction dfl;
          memset(&dfl, 0, sizeof(dfl));
          dfl.sa_handler = SIG_DFL;
          sigaction(s, &dfl, NULL);
        }
      } else {
        struct sigaction want;
        build_true_kernel_action(&n->act, &want);
        sigaction(s, &want, NULL);
      }
    } else if (slot->have_pre_kernel) {
      sigaction(s, &slot->pre_kernel, NULL);
    } else {
      struct sigaction dfl;
      memset(&dfl, 0, sizeof(dfl));
      dfl.sa_handler = SIG_DFL;
      sigaction(s, &dfl, NULL);
    }
    slot->installed = 0;
    slot->installed_variant = -1;
    slot->installed_kflags = 0;
  }

  atomic_store_explicit(&g_active, 0, memory_order_release);
}
