/*
 * libbox64dl: the guest half of the process-wide application signal action.
 *
 * WHAT LIVES HERE
 * ---------------
 * src/signal_actions.c (patch 0020) owns the immutable per-signal action, the
 * kernel dispositions and the "which kind is this" decision.  This file owns
 * everything that needs the *guest* view of a delivery:
 *
 *   - the per-thread active-delivery frame stack, which is how a guest handler
 *     that calls back into a native handler finds the REAL native ucontext_t of
 *     the delivery it is running inside (and how nested deliveries stay
 *     distinguishable);
 *   - the pending resume signal mask, which carries a guest uc_sigmask edit
 *     across the engine's own siglongjmp (patch 0005 restores M0/M1 but knows
 *     nothing about an edit the guest handler made);
 *   - the guest<->logical action conversion, including the exact kernel
 *     `rt_sigaction` layout and sigsetsize validation;
 *   - the two bridge directions:
 *       guest -> native   a stable, deduped guest-callable bridge for a NATIVE
 *                         action, so a guest can query it, call it (chaining)
 *                         and reinstall it without ever executing an x86
 *                         pointer on the CPU;
 *       native -> guest   the suspension wrapper the engine's dispatcher uses
 *                         before calling any application handler.
 *
 * FRAME LIFETIME: EXPLICIT SUSPEND, NEVER A DANGLING POINTER
 * ---------------------------------------------------------
 * A signal delivery can end in a guest resume `siglongjmp` that never returns
 * through the C frames of the handler, and a NATIVE application handler is
 * arbitrary code that may leave through a foreign longjmp as well.  Anything a
 * foreign longjmp could strand on the signal stack is therefore never *read*
 * afterwards:
 *
 *   - every frame entry carries a copy of the values it needs and an epoch;
 *   - `box64dl_signal_resume_mask_apply()` (called from the engine's resume
 *     hook in dynarec.c) and `box64dl_signal_guest_leave()` bump the epoch, so
 *     every abandoned frame becomes invalid in one store;
 *   - before calling ANY application handler whose return is not guaranteed,
 *     `box64dl_signal_guest_native_call*()` copies the whole fixed TLS frame
 *     array to a local, CLEARS the TLS array and bumps the epoch.  A foreign
 *     longjmp therefore leaves the TLS empty, never dangling.  On the ordinary
 *     return path only, the parent frames are restored when no other
 *     resume/invalidate happened meanwhile; a frame abandoned by the foreign
 *     jump is simply dropped.
 *
 * This file is grafted into the vendored box64 build (same compile flags as
 * src/signal_route.c), so it may use the engine's register macros and private
 * types.  It never edits the vendored tree: patches/0021 carries the call
 * sites.
 *
 * HONEST LIMITS (documented, not papered over)
 * --------------------------------------------
 *   - Outside an active delivery there is no real native ucontext_t.  A native
 *     3-argument handler reached from plain guest code therefore receives a
 *     SYNTHESIZED host ucontext carrying the guest's uc_sigmask/uc_stack/uc_link
 *     and a zeroed uc_mcontext.  It is enough for mask manipulation and for a
 *     handler that only reads si_*, but `setcontext()` on it is unsupported.
 *   - The guest siginfo_t is translated field by field (signo/errno/code/addr,
 *     pid/uid for the user-generated shapes); an exotic si_code payload the
 *     x86_64 and host layouts disagree about is not translated.
 *   - Signals 32/33 are the C library's internal signals.  The action module
 *     refuses them with ENOTSUP and this file has no second path: a guest
 *     registration for 32/33 is refused, never forwarded to a raw
 *     rt_sigaction that would overwrite the C library's own handler.  "Not
 *     tracked" is an honest refusal, not a silent raw overwrite.
 *   - The stable-bridge cache has a fixed capacity.  Past it a registration
 *     handing out an identity that a later query would not reproduce.
 *     The refusal is counted for diagnostics.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <ucontext.h>

#include "box64context.h"
#include "bridge.h"
#include "debug.h"
#include "emu/x64emu_private.h"
#include "regs.h"
#include "signals.h"
#include "libtools/signal_private.h"
#include "libbox64_signal_actions.h"

/* ------------------------------------------------------------------------ *
 * Active-delivery frames
 * ------------------------------------------------------------------------ */

#define BOX64DL_GUEST_FRAMES 8

typedef struct box64dl_guest_frame_s {
  int host_sig;
  /* The real original host ucontext_t of this delivery, owned by the kernel's
   * signal frame (or NULL for a synthetic, emulator-generated delivery).  It is
   * only dereferenced while the frame is live in the current epoch. */
  void* native_uc;
  /* Caller-owned copy of the action; valid for the whole delivery. */
  const box64dl_signal_action_t* action;
  uint64_t epoch;
} box64dl_guest_frame_t;

static __thread box64dl_guest_frame_t g_frames[BOX64DL_GUEST_FRAMES];
static __thread int g_depth;
/* Bumped whenever a jump may have abandoned frames.  A frame whose epoch is not
 * the current one is dead and is never read. */
static __thread uint64_t g_epoch = 1;
/* Bumped ONLY by a real invalidation (resume jump, frame teardown, over-deep
 * nesting).  A suspension wrapper records it before clearing the TLS and, on a
 * normal return, restores the parent frames only when it is unchanged.
 *
 * This is deliberately NOT the epoch: a suspension is not an abandonment and a
 * NESTED normal call must not poison its parent.  Execution reaching the code
 * after the application call proves this wrapper's C frame — and therefore its
 * saved parent frames — is still live; a genuine foreign jump never returns
 * here and simply leaves the TLS cleared. */
static __thread uint64_t g_invalidate_gen = 1;

static __thread int g_resume_mask_valid;
static __thread x64_sigset_t g_resume_mask;

/* Forward declaration: the guest->native bridges below call it, and it is
 * defined in the "application-call suspension wrapper" section. */
static void native_call_suspended(int host_sig, siginfo_t* info, void* native_uc,
                                  uint32_t flags, void (*fn)(void));

static void frames_invalidate(void) {
  g_depth = 0;
  memset(g_frames, 0, sizeof(g_frames));
  ++g_epoch;
  if (!g_epoch) g_epoch = 1;
  ++g_invalidate_gen;
  if (!g_invalidate_gen) g_invalidate_gen = 1;
}

/* Drop the top frame.  Idempotent and safe to call when no frame is active
 * (the synthetic emit path never enters one).  Called by the engine immediately
 * before every guest resume jump it initiates, and by the guest worker on the
 * normal return path. */
void box64dl_signal_guest_leave(void) {
  if (g_depth > 0) {
    --g_depth;
    g_frames[g_depth].native_uc = NULL;
    g_frames[g_depth].action = NULL;
    g_frames[g_depth].epoch = 0;
  }
}

/* Abandon every frame at once.  The resume hook calls this because a longjmp
 * out of a handler skips the normal `leave`, and the abandoned frames point at
 * a signal stack that is about to become unreachable. */
void box64dl_signal_guest_frames_invalidate(void) { frames_invalidate(); }

void box64dl_signal_guest_enter(int host_sig, void* native_uc,
                                const box64dl_signal_action_t* action) {
  if (g_depth < 0) g_depth = 0;
  if (g_depth >= BOX64DL_GUEST_FRAMES) {
    /* Out of nested frame slots.  The delivery still runs; the bridge
     * back-mapping simply refuses, which is the documented fallback (a
     * synthesized context) rather than a read of a stale one. */
    frames_invalidate();
  }
  box64dl_guest_frame_t* f = &g_frames[g_depth++];
  f->host_sig = host_sig;
  f->native_uc = native_uc;
  f->action = action;
  f->epoch = g_epoch;
}

/* 1 while a live delivery frame owns `native_uc` bookkeeping. */
int box64dl_signal_guest_frames_valid(void) { return g_depth > 0; }

/* The action of the innermost live delivery, or NULL outside one. */
const box64dl_signal_action_t* box64dl_signal_guest_action(void) {
  if (g_depth <= 0) return NULL;
  const box64dl_guest_frame_t* f = &g_frames[g_depth - 1];
  if (f->epoch != g_epoch) return NULL;
  return f->action;
}

/* The real native ucontext_t of the innermost live delivery, or NULL.  Only
 * valid while the delivery is on the stack — see the epoch check. */
void* box64dl_signal_guest_native_uc(void) {
  if (g_depth <= 0) return NULL;
  const box64dl_guest_frame_t* f = &g_frames[g_depth - 1];
  if (f->epoch != g_epoch) return NULL;
  return f->native_uc;
}

/* ------------------------------------------------------------------------ *
 * Signal-set conversion
 *
 * The x86_64 kernel and the host both encode the signal mask as a bit array
 * with bit i meaning signal i+1, and both keep words in the same order.  Only
 * the first 64 signals (one 64-bit word) can be named by a
 * `rt_sigaction`/`rt_sigprocmask` with sigsetsize == 8, which is the only size
 * the engine accepts.  Copy exactly that word and zero the remainder, so no
 * host-only bit can leak into a guest mask. */
/* ------------------------------------------------------------------------ */

static void x64_mask_to_host(const x64_sigset_t* in, sigset_t* out) {
  memset(out, 0, sizeof(*out));
  memcpy(out, in, sizeof(uint64_t));
}

static void host_mask_to_x64(const sigset_t* in, x64_sigset_t* out) {
  memset(out, 0, sizeof(*out));
  memcpy(out, in, sizeof(uint64_t));
}

/* ------------------------------------------------------------------------ *
 * Pending resume mask
 *
 * The engine's resume path (patch 0005) restores either the setjmp-time mask
 * (COMPAT) or the delivered mask (FAST).  Neither is the guest handler's own
 * uc_sigmask edit, so an edit that forced a resume would be silently dropped.
 * The handler records the edited mask here just before the jump; the resume
 * hook applies it after `siglongjmp` has returned, i.e. after COMPAT's own
 * restore, so the guest's decision wins under both policies. */
/* ------------------------------------------------------------------------ */

void box64dl_signal_guest_note_resume_mask(const x64_sigset_t* mask) {
  if (!mask) {
    g_resume_mask_valid = 0;
    return;
  }
  g_resume_mask = *mask;
  g_resume_mask_valid = 1;
}

/* Called from the engine's resume hook (dynarec.c) with the signal mask
 * unblocked.  Invalidate the abandoned frames before doing anything else:
 * their native ucontext_t belongs to the signal frame the jump just left. */
void box64dl_signal_resume_mask_apply(void) {
  /* The frames are invalid unconditionally: whatever jumped, their native
   * ucontext_t belongs to the signal frame it just left. */
  frames_invalidate();
  /* The mask, however, is applied ONLY when the handler actually recorded an
   * edit.  Applying a zero/stale mask on every engine resume would clobber the
   * mask the engine's own policy just restored (0005). */
  if (!g_resume_mask_valid) return;
  g_resume_mask_valid = 0;
  sigset_t host;
  x64_mask_to_host(&g_resume_mask, &host);
  int saved = errno;
  sigprocmask(SIG_SETMASK, &host, NULL);
  errno = saved;
}

/* ------------------------------------------------------------------------ *
 * Guest <-> logical action conversion
 * ------------------------------------------------------------------------ */

/* The x86_64 kernel's `struct sigaction` as a guest sees it: handler/flags/
 * restorer/mask, 32 bytes.  Only the first 8 mask bytes are meaningful for the
 * sigsetsize == 8 the engine accepts.  See include/libbox64_signal_actions.h
 * ("THE KERNEL LAYOUT IS NOT THIS STRUCTURE"). */
static void decode_kernel_action(const x64_sigaction_restorer_t* in,
                                 box64dl_signal_action_t* out) {
  memset(out, 0, sizeof(*out));
  uintptr_t h = (uintptr_t)in->_u._sa_handler;
  out->kind = (h == 0)   ? BOX64_SIGNAL_ACTION_DEFAULT
              : (h == 1) ? BOX64_SIGNAL_ACTION_IGNORE
                         : (GetNativeFnc(h) ? BOX64_SIGNAL_ACTION_NATIVE
                                            : BOX64_SIGNAL_ACTION_GUEST);
  out->handler = (out->kind == BOX64_SIGNAL_ACTION_NATIVE)
                     ? (uintptr_t)GetNativeFnc(h)
                 : (out->kind == BOX64_SIGNAL_ACTION_GUEST) ? h
                                                            : 0;
  out->restorer = (uintptr_t)in->sa_restorer;
  memset(&out->mask, 0, sizeof(out->mask));
  memcpy(&out->mask, &in->sa_mask, sizeof(uint64_t));
  /* Unsigned 32-bit widening: SA_RESETHAND is 0x80000000 and would sign-extend
   * through a plain int -> uint64_t conversion. */
  out->flags = (uint64_t)(uint32_t)in->sa_flags;
  out->generation = 0;
}

/* x64_sigaction_restorer_t is the HOST-side carrier the engine uses for the
 * x86_64 kernel layout; it ends in a HOST sigset_t (128 bytes), so
 * sizeof(x64_sigaction_restorer_t) == 152.  The buffer this function writes
 * through is the guest's `struct sigaction` as the x86_64 kernel defines it:
 * handler, flags, restorer, and an 8-byte mask (sigsetsize == 8) — 32 bytes
 * total.  A memset of sizeof(*out) here would write 120 bytes past the end of
 * that guest buffer (the rt_sigaction oldact a guest passes to
 * my_syscall_rt_sigaction), corrupting the caller's stack.  Clear exactly the
 * meaningful kernel span: everything up to and including the 64-bit mask word
 * (24 + 8 == 32), never the host-only sigset_t tail. */
static void encode_kernel_action(const box64dl_signal_action_t* a,
                                 x64_sigaction_restorer_t* out) {
  memset(out, 0, offsetof(x64_sigaction_restorer_t, sa_mask) + sizeof(uint64_t));
  if (a->kind == BOX64_SIGNAL_ACTION_DEFAULT)
    out->_u._sa_handler = (sighandler_t)0;
  else if (a->kind == BOX64_SIGNAL_ACTION_IGNORE)
    out->_u._sa_handler = (sighandler_t)1;
  else
    out->_u._sa_handler = (sighandler_t)a->handler;
  out->sa_flags = (uint32_t)a->flags;
  out->sa_restorer = (void (*)(void))a->restorer;
  memcpy(&out->sa_mask, &a->mask, sizeof(uint64_t));
}

/* The libc `struct sigaction` (152 bytes: union at 0, sigset_t at 8, uint32
 * sa_flags at 136, sa_restorer at 144) — what my_sigaction() and the
 * `sigaction` family see. */
void box64dl_signal_guest_encode_libc(const box64dl_signal_action_t* a,
                                      x64_sigaction_t* out) {
  memset(out, 0, sizeof(*out));
  int siginfo = ((uint32_t)a->flags & SA_SIGINFO) != 0;
  if (a->kind == BOX64_SIGNAL_ACTION_DEFAULT) {
    out->_u._sa_handler = SIG_DFL;
  } else if (a->kind == BOX64_SIGNAL_ACTION_IGNORE) {
    out->_u._sa_handler = SIG_IGN;
  } else if (siginfo) {
    out->_u._sa_sigaction = (void (*)(int, siginfo_t*, void*))a->handler;
  } else {
    out->_u._sa_handler = (sighandler_t)a->handler;
  }
  out->sa_mask = a->mask;
  out->sa_flags = (uint32_t)a->flags;
  out->sa_restorer = (void (*)(void))a->restorer;
}

void box64dl_signal_guest_decode_libc(const x64_sigaction_t* in,
                                      box64dl_signal_action_t* out) {
  memset(out, 0, sizeof(*out));
  uintptr_t h = (uintptr_t)in->_u._sa_handler;
  out->kind = (h == 0)   ? BOX64_SIGNAL_ACTION_DEFAULT
              : (h == 1) ? BOX64_SIGNAL_ACTION_IGNORE
                         : (GetNativeFnc(h) ? BOX64_SIGNAL_ACTION_NATIVE
                                            : BOX64_SIGNAL_ACTION_GUEST);
  out->handler = (out->kind == BOX64_SIGNAL_ACTION_NATIVE)
                     ? (uintptr_t)GetNativeFnc(h)
                 : (out->kind == BOX64_SIGNAL_ACTION_GUEST) ? h
                                                            : 0;
  out->restorer = (uintptr_t)in->sa_restorer;
  out->mask = in->sa_mask;
  out->flags = (uint64_t)(uint32_t)in->sa_flags;
  out->generation = 0;
}

/* Kernel `rt_sigaction`: the guest hands a restorer-layout struct and a
 * sigsetsize.  The x86_64 kernel accepts exactly sizeof(sigset_t) == 8; every
 * other size is EINVAL, which is what tests/guest/signal_actions_guest.c's
 * g_action_query_size() probes (4 and 16 refused, 8 accepted). */
int box64dl_signal_guest_decode_rt(const x64_sigaction_restorer_t* in,
                                   int sigsetsize, box64dl_signal_action_t* out) {
  if (sigsetsize != 8) {
    errno = EINVAL;
    return -1;
  }
  decode_kernel_action(in, out);
  return 0;
}

int box64dl_signal_guest_encode_rt(const box64dl_signal_action_t* a,
                                   x64_sigaction_restorer_t* out) {
  encode_kernel_action(a, out);
  return 0;
}

/* ------------------------------------------------------------------------ *
 * guest -> native: the stable, guest-callable bridge for a NATIVE action
 *
 * A guest that queries a signal action must get something it can (a) compare
 * stably across queries, (b) call, and (c) reinstall so the round trip lands
 * back on the NATIVE action rather than on a guest one.  The bridge address is
 * that value: it is a real executable guest address (box64's bridge thunk) with
 * a fixed identity per (native function, arity).
 *
 * The wrapper is chosen by arity, and the wrapper IS part of the bridge key, so
 * a 1-argument and a 3-argument registration of the same native function can
 * never alias each other.
 *
 * WHY NOT AddCheckBridge: its lookup walks the bridge khash without holding the
 * bridge lock (CheckBridgedWrapper), so two threads racing on the same key can
 * both miss and both AddBridge, and a reader can observe either brick.  This
 * module keeps its own cache under a local mutex instead: the (handler, arity)
 * lookup and, on a miss, the AddBridge that fills the slot happen in one
 * critical section, so exactly one bridge identity exists per key.
 *
 * The cache is BOUNDED.  Past its capacity the export FAILS (0) rather than
 * handing out an undeduped second identity for the same native function: a
 * caller that cannot get a stable bridge must report the failure, not silently
 * publish an identity that a later query would not reproduce.
 * ------------------------------------------------------------------------ */

#define BOX64DL_BRIDGE_CACHE 128

typedef struct box64dl_bridge_cache_s {
  uintptr_t handler;
  unsigned arity; /* 1 = SA_SIGINFO three-argument wrapper */
  uintptr_t bridge;
} box64dl_bridge_cache_t;

static box64dl_bridge_cache_t g_bcache[BOX64DL_BRIDGE_CACHE];
static unsigned g_bcache_used;
static pthread_mutex_t g_bcache_mutex = PTHREAD_MUTEX_INITIALIZER;
static unsigned long g_bcache_overflow;

/* Minimal x86_64 view of siginfo_t: signo, errno, code, then the union the
 * kernel lays down at offset 16 (the fault address for the fault shapes, the
 * pid/uid pair for the user-generated ones).
 *
 * The fields are deliberately NOT named si_*: <signal.h> defines the si_* names
 * as macros expanding into _sifields, so a member called si_signo or si_addr
 * would be textually rewritten into a member of a union this struct does not
 * have.  The x86_64 layout is reproduced positionally instead.  The HOST
 * struct's si_* members on the right-hand side are the real macros and must
 * stay that way. */
typedef struct box64dl_x64_siginfo_s {
  int32_t signo;
  int32_t err;
  int32_t code;
  int32_t __pad0;
  union {
    void* fault_addr;
    struct {
      int32_t pid;
      uint32_t uid;
    } user;
    char pad[112];
  } u;
} box64dl_x64_siginfo_t;

static void x64_info_to_host(const box64dl_x64_siginfo_t* in, int host_sig,
                             siginfo_t* out) {
  memset(out, 0, sizeof(*out));
  out->si_signo = host_sig;
  out->si_errno = in->err;
  out->si_code = in->code;
  out->si_addr = in->u.fault_addr;
  if (in->code <= 0) {
    /* SI_USER / SI_QUEUE: the pid/uid pair lives at the same offset. */
    out->si_pid = (pid_t)in->u.user.pid;
    out->si_uid = (uid_t)in->u.user.uid;
  }
}

/* 1-argument native handler: the guest's single argument is the signal number.
 * The application call goes through the same suspension as the 3-argument one:
 * a foreign longjmp out of the handler must not strand the delivery frames. */
static void box64dl_bridge_native_1(x64emu_t* emu, uintptr_t fnc) {
  void (*f)(int) = (void (*)(int))fnc;
  int saved_errno = errno;
  (void)emu;
  native_call_suspended((int32_t)R_RDI, NULL, NULL, 0, (void (*)(void))f);
  errno = saved_errno;
}

/* 3-argument (SA_SIGINFO) native handler.
 *
 * Inside an active delivery the guest is calling back into the native handler
 * from its own signal handler, and the x86 context it passes IS this delivery's
 * context: we hand the native handler the REAL host ucontext_t and copy the
 * guest's mask edits into it first, then copy the native handler's mask edits
 * back out.  Outside a delivery there is no real context; the host ucontext is
 * synthesized from the guest's mask/stack only (see the file header) and no
 * edit is propagated anywhere.
 *
 * The native handler is arbitrary application code and may leave through a
 * foreign longjmp; the frame suspension inside native_call_suspended() is what
 * keeps the TLS from dangling when it does. */
static void box64dl_bridge_native_3(x64emu_t* emu, uintptr_t fnc) {
  void (*f)(int, siginfo_t*, void*) = (void (*)(int, siginfo_t*, void*))fnc;
  int x64_sig = (int)(int32_t)R_RDI;
  const box64dl_x64_siginfo_t* gi = (const box64dl_x64_siginfo_t*)R_RSI;
  x64_ucontext_t* guc = (x64_ucontext_t*)R_RDX;
  int host_sig = signal_from_x64(x64_sig);
  int saved_errno = errno;
  (void)emu;

  siginfo_t info;
  if (gi)
    x64_info_to_host(gi, host_sig, &info);
  else
    memset(&info, 0, sizeof(info));

  ucontext_t synth;
  ucontext_t* real = (ucontext_t*)box64dl_signal_guest_native_uc();
  if (guc) {
    if (real) {
      /* The guest's edit is the app's decision: land it on the real context
       * before the native handler sees it. */
      sigset_t m;
      x64_mask_to_host(&guc->uc_sigmask, &m);
      real->uc_sigmask = m;
    } else {
      memset(&synth, 0, sizeof(synth));
      x64_mask_to_host(&guc->uc_sigmask, &synth.uc_sigmask);
      synth.uc_stack.ss_sp = guc->uc_stack.ss_sp;
      synth.uc_stack.ss_size = guc->uc_stack.ss_size;
      synth.uc_stack.ss_flags = guc->uc_stack.ss_flags;
      synth.uc_link = (ucontext_t*)guc->uc_link;
      real = &synth;
    }
  } else if (!real) {
    memset(&synth, 0, sizeof(synth));
    real = &synth;
  }

  /* The wrapper owns its own suspension: a foreign longjmp out of `f` must not
   * leave the TLS pointing at this delivery's signal stack. */
  native_call_suspended(host_sig, &info, real, SA_SIGINFO, (void (*)(void))f);

  /* Only reached on a normal return. */
  if (guc) {
    void* back = box64dl_signal_guest_native_uc();
    if (back) {
      /* Propagate the native handler's mask edit back to the guest context the
       * guest handler is looking at, so a chained native handler's decision is
       * what the guest observes. */
      host_mask_to_x64(&((ucontext_t*)back)->uc_sigmask, &guc->uc_sigmask);
    }
  }
  errno = saved_errno;
}

static void* bridge_wrapper_for(unsigned arity) {
  return arity ? (void*)box64dl_bridge_native_3
               : (void*)box64dl_bridge_native_1;
}

/* Guest-visible value for an action: the original guest pointer for GUEST, a
 * stable bridge for NATIVE, SIG_DFL/SIG_IGN for DEFAULT/IGNORE.  Never a raw
 * native pointer: returning one would let the guest execute host code on the
 * CPU (and would not survive a round trip). */
unsigned long box64dl_signal_guest_export(int host_sig,
                                          const box64dl_signal_action_t* a) {
  (void)host_sig;
  switch (a->kind) {
    case BOX64_SIGNAL_ACTION_DEFAULT: return 0;
    case BOX64_SIGNAL_ACTION_IGNORE: return 1;
    case BOX64_SIGNAL_ACTION_GUEST: return a->handler;
    case BOX64_SIGNAL_ACTION_NATIVE:
    default: break;
  }
  /* A NATIVE action with no handler is not a default action: it is an
   * unusable snapshot.  Report it rather than exporting 0, which a caller
   * would read as SIG_DFL and silently install a different action. */
  if (!a->handler) {
    errno = EINVAL;
    return 0;
  }
  if (!my_context || !my_context->system) {
    errno = ENOSYS;
    return 0;
  }

  unsigned arity = ((uint32_t)a->flags & SA_SIGINFO) ? 1u : 0u;

  /* Lookup and, on a miss, the AddBridge that fills the slot, under one lock:
   * two threads racing on the same (handler, arity) can never publish two
   * identities for it. */
  pthread_mutex_lock(&g_bcache_mutex);
  for (unsigned i = 0; i < g_bcache_used; ++i) {
    if (g_bcache[i].handler == a->handler && g_bcache[i].arity == arity) {
      uintptr_t b = g_bcache[i].bridge;
      pthread_mutex_unlock(&g_bcache_mutex);
      return b;
    }
  }
  if (g_bcache_used >= BOX64DL_BRIDGE_CACHE) {
    /* Bounded: refusing is the honest answer, because an undeduped second
     * identity for the same native function would break the query round trip. */
    ++g_bcache_overflow;
    pthread_mutex_unlock(&g_bcache_mutex);
    errno = ENOMEM;
    return 0;
  }

  uintptr_t br =
      AddBridge(my_context->system, (wrapper_t)bridge_wrapper_for(arity),
                (void*)a->handler, 0, "signal_action");
  if (br) {
    unsigned slot = g_bcache_used++;
    g_bcache[slot].handler = a->handler;
    g_bcache[slot].arity = arity;
    g_bcache[slot].bridge = br;
  } else {
    errno = ENOMEM;
  }
  pthread_mutex_unlock(&g_bcache_mutex);
  return br;
}

unsigned long box64dl_signal_guest_bridge_overflows(void) {
  /* The counter is written under the cache mutex, so read it under the same
   * lock instead of racing it. */
  pthread_mutex_lock(&g_bcache_mutex);
  unsigned long n = g_bcache_overflow;
  pthread_mutex_unlock(&g_bcache_mutex);
  return n;
}

/* Reverse of export: a handler value coming back from the guest that is one of
 * our bridges is a NATIVE action whose real function is the bridge's native
 * target.  Anything else is a guest address.  This is what makes a
 * query -> reinstall round trip restore kind NATIVE instead of storing the
 * bridge address as a guest handler. */
int box64dl_signal_guest_import(uintptr_t handler, uintptr_t* native_out) {
  if (native_out) *native_out = 0;
  if (!handler || handler == 1) return 0;
  void* nat = GetNativeFnc(handler);
  if (!nat) return 0;
  if (native_out) *native_out = (uintptr_t)nat;
  return 1;
}

/* ------------------------------------------------------------------------ *
 * native -> guest: the application-call suspension wrapper
 *
 * Two callers use this:
 *   - the action module's NATIVE delivery path, through the approved
 *     `box64dl_native_call_hook` function pointer, so the delivery does not
 *     depend on this translation unit at link time;
 *   - this file's own guest->native bridges.
 *
 * It does NOT apply the application mask and does NOT touch errno: the caller
 * owns those (the module applies the mask once, before the hook).  Its one job
 * besides invoking by arity is the frame suspension described in the file
 * header.
 * ------------------------------------------------------------------------ */

/* Core suspension wrapper.  `fn` is the application handler itself. */
static void native_call_suspended(int host_sig, siginfo_t* info, void* native_uc,
                                  uint32_t flags, void (*fn)(void)) {
  box64dl_guest_frame_t local[BOX64DL_GUEST_FRAMES];
  int depth = g_depth;
  memcpy(local, g_frames, sizeof(local));
  uint64_t gen = g_invalidate_gen;
  g_depth = 0;
  memset(g_frames, 0, sizeof(g_frames));

  if (flags & SA_SIGINFO)
    ((void (*)(int, siginfo_t*, void*))fn)(host_sig, info, native_uc);
  else
    ((void (*)(int))fn)(host_sig);

  /* Ordinary return only: a foreign longjmp never reaches here and leaves the
   * TLS empty, which is exactly what keeps it from dangling.
   *
   * A nested normal call must NOT poison its parent: this C frame is provably
   * still live (we are executing in it), so the parent frames it saved are too,
   * and the only reason not to reinstate them is a genuine invalidation in
   * between.  That is what g_invalidate_gen distinguishes from the epoch. */
  if (g_invalidate_gen == gen && g_depth == 0) {
    memcpy(g_frames, local, sizeof(local));
    g_depth = depth;
    for (int i = 0; i < depth; ++i) g_frames[i].epoch = g_epoch;
  }
}

/* The approved hook target (4 arguments, frozen for the action module):
 *
 *   void box64dl_signal_guest_native_call(const box64dl_signal_action_t*,
 *                                         int host_sig, siginfo_t*, void*);
 *
 * The action module calls it for a NATIVE action with the REAL delivered
 * info/ucontext, AFTER it has applied the application mask and saved errno.
 * `action` supplies both the handler and the flags that select the arity. */
void box64dl_signal_guest_native_call(const box64dl_signal_action_t* action,
                                      int host_sig, siginfo_t* info,
                                      void* native_uc) {
  if (!action || !action->handler) return;
  native_call_suspended(host_sig, info, native_uc, (uint32_t)action->flags,
                        (void (*)(void))action->handler);
}

/* ------------------------------------------------------------------------ *
 * Reset (quiesced startup/fini only)
 * ------------------------------------------------------------------------ */

/* Reset the pending resume mask, the frame stack and the stable-bridge cache.
 * The caller must have quiesced deliveries: the bridge cache is the identity a
 * guest may still hold, so resetting it while a guest could call a cached value
 * would let the same native function acquire a second identity.  Engine init
 * calls this before the first guest runs and fini after deliveries stop. */
void box64dl_signal_guest_reset(void) {
  frames_invalidate();
  g_resume_mask_valid = 0;
  memset(&g_resume_mask, 0, sizeof(g_resume_mask));
  pthread_mutex_lock(&g_bcache_mutex);
  memset(g_bcache, 0, sizeof(g_bcache));
  g_bcache_used = 0;
  g_bcache_overflow = 0;
  pthread_mutex_unlock(&g_bcache_mutex);
}

/* ------------------------------------------------------------------------ *
 * Guest-side mask edit on the plain-return path
 * ------------------------------------------------------------------------ */

/* The guest-side mask edit the engine must honour when it resumes a guest that
 * changed only its ucontext mask (no mcontext change, so the resume path does
 * not run).  Applying it here reproduces, on the bridge frame, what the kernel's
 * own sigreturn would have done for a real frame. */
void box64dl_signal_guest_apply_mask_edit(const x64_sigset_t* edited) {
  if (!edited) return;
  sigset_t host;
  x64_mask_to_host(edited, &host);
  int saved = errno;
  sigprocmask(SIG_SETMASK, &host, NULL);
  errno = saved;
}
