/*
 * libbox64dl: the process-wide application signal action set.
 *
 * WHAT THIS IS
 * ------------
 * One *current* application action per signal, captured from the process's real
 * dispositions before the engine installs its own kernel handlers, plus a
 * controlled native API the embedder uses after init to read or replace that
 * action. The embedder never writes the raw kernel disposition behind the
 * engine's back: every kernel disposition on the signals this module owns
 * belongs to src/signal_actions.c, which keeps the immutable action snapshot and
 * the kernel handler table consistent under concurrent delivery.
 *
 * KIND
 * ----
 *   DEFAULT  the kernel default disposition. The four engine fatal signals
 *            (SIGSEGV/SIGBUS/SIGILL/SIGABRT) apply it terminally after every
 *            repair: unblock self, re-raise on the same thread, do not return.
 *            For any other signal the kernel's own default table is honoured —
 *            default-ignore (SIGCHLD, SIGCONT, SIGURG, SIGWINCH) returns, the
 *            default-stop signals raise with SIG_DFL and continue after
 *            SIGCONT, and only a true termination terminates.
 *   IGNORE   a no-op. For a synchronous hardware fault this is not a promise
 *            that the fault is repaired: the instruction is undefined on
 *            Linux/POSIX and may re-execute. The engine's own repair always
 *            runs first.
 *   NATIVE   a *native* C handler, called with the real original siginfo_t and
 *            host ucontext_t, by SIGINFO arity (3 arguments) or 1 argument
 *            otherwise. No emu is required or fabricated for it. The ucontext_t
 *            may be NULL for a synthesized delivery (see
 *            box64dl_signal_deliver()); a NULL is passed through as NULL rather
 *            than inventing a frame.
 *   GUEST    a *guest* (x86-64) handler address. It is a guest address and must
 *            never be called as a native C function; guest dispatch goes
 *            through the engine's guest worker.
 *
 * SCOPE AND CONCURRENCY
 * ---------------------
 * There is no "cold only" restriction: the getter, the native setter and
 * delivery are all supported concurrently with each other and with guest
 * execution. Readers take no userspace lock: each reader section is a short
 * counted region which temporarily defers signal delivery on its own thread and
 * releases its reference before any application callback, so no reference is
 * ever held across a handler. A one-shot (SA_RESETHAND) consumption is a single
 * compare-and-swap that can never erase a newer replacement, and writers are
 * serialized against each other by a mutex the module only ever *tries* to take.
 * Every action node is preallocated at init, so the module never allocates or
 * frees memory on a delivery path.
 *
 * The engine's fini is the documented exception: the caller must quiesce
 * deliveries first (see box64dl_signal_actions_fini()).
 *
 * THE KERNEL LAYOUT IS NOT THIS STRUCTURE
 * ---------------------------------------
 * `flags` and `mask` are the *logical application* values: exactly what the
 * application asked for (SA_ONSTACK, SA_RESTART, SA_NODEFER, SA_SIGINFO,
 * SA_RESETHAND, SA_NOCLDSTOP, SA_NOCLDWAIT, sa_mask bits). The module derives the
 * kernel flags it actually needs; it does not force SA_SIGINFO, SA_NODEFER or
 * SA_RESTART onto the recorded application action, and `handler` is never the
 * module's own kernel entry point.
 *
 * `flags` is a uint64_t holding an *unsigned* 32-bit sa_flags. Every producer
 * widens through `(uint64_t)(uint32_t)sa_flags` and every consumer compares the
 * low 32 bits (`(uint32_t)flags & SA_RESETHAND`). SA_RESETHAND is 0x80000000: a
 * plain int -> uint64_t widening would sign-extend it to 0xffffffff80000000 and
 * break both comparison and kernel reconciliation.
 *
 * RESERVED / REJECTED SIGNALS
 * ---------------------------
 * host_sig 32 and 33 are the C library's internal pthread signals (SIGCANCEL /
 * SIGSETXID). glibc's and musl's sigaction() and sigprocmask() may silently
 * filter or reject them, and the engine's guest-registration path for them uses
 * raw rt_sigaction/rt_sigprocmask instead. This module therefore does not track,
 * capture or install 32/33: every entry point rejects them with ENOTSUP. That
 * protects the host C library's internal handlers instead of overwriting them
 * through an interface that does not faithfully represent them.
 *
 * Consequence, stated plainly: this module does NOT claim 1..64 fidelity. Its
 * domain is 1..31 and 34..64. BOX64DL_SIGNAL_MAX is 64 so a caller can ask about
 * 32/33 and get a documented refusal rather than a spurious out-of-range error.
 *
 * ERRNO CONTRACT (harmonized with Linux rt_sigaction)
 * ---------------------------------------------------
 *   EINVAL       host_sig < 1, > 64, or SIGKILL/SIGSTOP for a *set*; a bad `act`
 *                (e.g. sa_handler == SIG_ERR). Linux rejects SIGKILL/SIGSTOP for
 *                rt_sigaction outright, so the setter does too; the getter still
 *                answers for them (they are valid signals whose action can only
 *                ever be DEFAULT).
 *   ENOTSUP      host_sig is 32 or 33 (see above).
 *   EFAULT       a required output pointer is NULL.
 *   ENOSYS       the module is not initialized, or has been finalized.
 *   EWOULDBLOCK  a writer refused to proceed because the writer mutex is held:
 *                returned BEFORE any state is touched.
 *   ENOMEM       the bounded node supply is exhausted: the fixed preallocated
 *                pool (512 pairs) holds every simultaneously rooted node, but a
 *                writer stream that never reaches a quiescent moment (readers
 *                keep the grace counter non-zero) cannot have its retired pairs
 *                recycled and is refused here. A clean, documented refusal —
 *                never a silent drop.
 *   EPERM        the module cannot guarantee memory safety for this call.
 *                From init(): a kernel disposition could not be captured or a
 *                dispatcher could not be installed, or the reader window cannot
 *                be bounded on this target. init() is transactional and rolls
 *                every partial install back before returning this, so the
 *                process is left exactly as it was and the module stays inactive
 *                (every entry point then reports ENOSYS). It never fabricates a
 *                DEFAULT for an action it failed to capture.
 *                From box64_signal_action_get(): the temporary signal deferral
 *                that bounds its reader window could not be established or
 *                restored. The getter refuses rather than run an unbounded
 *                (interruptible) reader section.
 */

#ifndef LIBBOX64_SIGNAL_ACTIONS_H
#define LIBBOX64_SIGNAL_ACTIONS_H

#include <signal.h>
#include <stdint.h>

/* Same macro contract as include/libbox64.h (which already defines it): the
 * public entry points must be dynamically visible because the engine is built
 * -fvisibility=hidden and src/libbox64dl.map only globalizes symbols by name. */
#ifndef BOX64_API
#define BOX64_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------ *
 * Kind
 * ------------------------------------------------------------------------ */

#define BOX64_SIGNAL_ACTION_DEFAULT 0
#define BOX64_SIGNAL_ACTION_IGNORE  1
#define BOX64_SIGNAL_ACTION_NATIVE  2
#define BOX64_SIGNAL_ACTION_GUEST   3

/* Logical application flags are the host's own <signal.h> SA_* bits: no separate
 * numbering is invented here. */

/* ------------------------------------------------------------------------ *
 * Public action snapshot
 * ------------------------------------------------------------------------ */

typedef struct box64_signal_action_s {
  /* BOX64_SIGNAL_ACTION_* — the one field that says what `handler` means. */
  int kind;
  int _reserved0; /* explicit padding: deterministic layout, no uninit bytes */
  /* The ORIGINAL application handler address: the native C function for NATIVE,
   * the guest x86-64 address for GUEST, 0 for DEFAULT/IGNORE. This is never the
   * module's kernel entry point, and a GUEST value must not be called as a
   * native function. */
  uintptr_t handler;
  /* The ORIGINAL application restorer, 0 when the application had none. For a
   * GUEST action this is a guest x86-64 address: it is never installed as a
   * kernel restorer and never executed as native code. Guest dispatch bridges it
   * when it needs one. */
  uintptr_t restorer;
  /* Logical application mask: the pre-delivery mask the application asked for.
   * The self bit a delivery adds when SA_NODEFER is clear is NOT part of it. */
  sigset_t mask;
  /* Logical application sa_flags, widened as (uint64_t)(uint32_t) — see the file
   * header. Compare the low 32 bits. */
  uint64_t flags;
  /* Per-signal generation: bumped by every successful publication on this
   * signal. 0 means "never published by this module". */
  uint64_t generation;
} box64_signal_action_t;

/* Internal alias: same type, same layout, so the internal and public entry
 * points accept the same snapshot without a conversion step. */
typedef box64_signal_action_t box64dl_signal_action_t;

/* ------------------------------------------------------------------------ *
 * Public API (box64_* — the embedder's controlled native surface)
 * ------------------------------------------------------------------------ */

/* Read the current action for `host_sig`.
 *
 *   out->kind == DEFAULT/IGNORE/NATIVE/GUEST says what `handler` is. A GUEST
 *   `handler` (and its `restorer`) is a guest address and is NOT native
 *   callable.
 *
 * Returns 0 and fills *out, or -1 with errno set: EFAULT (out is NULL), EINVAL
 * (out of range), ENOTSUP (32/33), ENOSYS (not initialized), EPERM (the reader
 * window could not be bounded; see below). SIGKILL and SIGSTOP are answered as
 * DEFAULT, matching the fact that no handler can be installed for them.
 *
 * ASYNC-SIGNAL-SAFETY, PRECISELY. It never allocates, never takes a userspace
 * lock, and never calls an application callback. It is safe to call from a
 * signal handler, and it is async-signal-safe in the POSIX sense: its only
 * non-computation work is pthread_sigmask(), which POSIX lists as
 * async-signal-safe (a syscall, no userspace lock).
 *
 * The read is a short critical section which DEFERS signal delivery on the
 * calling thread rather than pretending to be uninterruptible: all catchable
 * signals are blocked for the few instructions that take and release the
 * reader reference and copy the snapshot, and the caller's mask is restored
 * afterwards (on success errno is restored too, exactly as found). A signal that
 * arrives in that interval is delivered immediately after the restore, never
 * lost and never observed while the reference is held.
 *
 * This deferral is what keeps the reference balanced even if an application
 * handler leaves its frame by a foreign longjmp: without it, an interrupt inside
 * the section would abandon the reference and permanently disable the module's
 * node reclamation, eventually refusing setters with ENOMEM. If the mask cannot
 * be set at all, the call fails with EPERM rather than run unbounded.
 *
 * Note the honest boundary: SIGKILL and SIGSTOP cannot be blocked, but neither
 * can longjmp out of this thread.
 *
 * ASYNCHRONOUS CANCELLATION IS OUTSIDE THE CONTRACT. The bounded window defers
 * *signal delivery* on the calling thread; it cannot bound pthread_cancel() /
 * asynchronous cancellation, thread exit, or any other mechanism that unwinds a
 * thread out of reader_enter()..reader_exit(). A thread asynchronously cancelled
 * inside a reader section may therefore leave its reference unbalanced, exactly
 * like a foreign longjmp would without the window. Callers that use asynchronous
 * cancellation must keep it away from these calls. */
BOX64_API int box64_signal_action_get(int host_sig,
                                      box64_signal_action_t* out);

/* Install a NATIVE action for `host_sig` from a real `struct sigaction`.
 *
 *   act == NULL   query only (Linux semantics); nothing changes, and *old is
 *                 filled when non-NULL. act == NULL with old == NULL is a
 *                 successful no-op.
 *   act != NULL   validate and install; *old (when non-NULL) receives the
 *                 replaced action.
 *
 * The recorded logical action is exactly what `act` asked for: application
 * `sa_flags` (low 32 bits; SA_RESETHAND is honoured through the one-shot path)
 * and `sa_mask`. Kernel details the caller cannot express this way — SA_NODEFER /
 * SA_SIGINFO / SA_RESTART reconciliation, the module's own entry variants — stay
 * the module's business. `sa_restorer` is recorded but never installed as a
 * kernel restorer.
 *
 * ASYNC-SIGNAL-SAFETY, HONESTLY: this function is NOT guaranteed
 * async-signal-safe and must not be relied on from inside a signal handler. Its
 * runtime path is bounded — it never takes memory from the allocator (all nodes
 * are preallocated at init) and it only ever *tries* the writer mutex, returning
 * EWOULDBLOCK before touching any state when that try fails. There is no
 * per-thread handler-depth latch and no refusal state that can outlive a call: a
 * native handler that leaves its signal frame by a foreign longjmp leaves
 * nothing behind that would refuse a later call.
 *
 * Returns 0 on success, or -1 with errno set per the header's ERRNO CONTRACT. */
BOX64_API int box64_signal_action_set_native(int host_sig,
                                             const struct sigaction* act,
                                             box64_signal_action_t* old);

/* ------------------------------------------------------------------------ *
 * Internal engine seam (grafted into the engine build; not a public ABI)
 * ------------------------------------------------------------------------ */

/* The internal entry points hand control to the engine, so they name its guest
 * type. A repeated identical typedef is legal C11; the guard lets the engine's
 * real x64emu.h define it too. */
#ifndef BOX64DL_X64EMU_FORWARD_DECLARED
#define BOX64DL_X64EMU_FORWARD_DECLARED
typedef struct x64emu_s x64emu_t;
#endif

/* Entry variant: which kernel handler was chosen for the delivery. The kernel
 * picks the variant, so the restart/stack semantics of the delivered frame match
 * the snapshot the reader copied from the matching pointer slot. */
#define BOX64DL_SIGNAL_VARIANT_ONSTACK 1
#define BOX64DL_SIGNAL_VARIANT_RESTART 2
#define BOX64DL_SIGNAL_VARIANT_MASK    3
#define BOX64DL_SIGNAL_VARIANTS        4

/* Tracked signal range. See "RESERVED / REJECTED SIGNALS" above: 32 and 33 are
 * inside the range only so they can be rejected with ENOTSUP. */
#define BOX64DL_SIGNAL_MIN 1
#define BOX64DL_SIGNAL_MAX 64

/* Internal get: identical contract to box64_signal_action_get(), including the
 * errno values, except that `out` may be NULL to ask "is this signal tracked?"
 * (still returns 0). */
int box64dl_signal_action_get(int host_sig, box64dl_signal_action_t* out);

/* Internal set: install `act` (any kind — also DEFAULT/IGNORE/GUEST) and report
 * the replaced action in `old` (may be NULL). NULL `act` with non-NULL `old` is a
 * query, exactly like the public setter. Same errno contract. */
int box64dl_signal_action_set(int host_sig, const box64dl_signal_action_t* act,
                              box64dl_signal_action_t* old);

/* Capture the process's current (pre-engine) application actions and install the
 * engine dispatcher on the four engine signals. Called exactly once, from the
 * engine's init_signal_helper(), while the application's own actions are still the
 * process dispositions — that capture IS the "before engine init" snapshot. The
 * host does not call this; engine init owns it.
 *
 * TRANSACTIONAL: returns 0 only when the whole startup transaction succeeded —
 * every tracked signal's action was captured AND every dispatcher that capture
 * requires (the four mandatory engine signals, plus every tracked signal whose
 * captured action is NATIVE or GUEST) was installed. If any capture or install
 * fails, every partial install is rolled back to the captured pre-init action
 * and the function returns -1; the module then stays inactive (active() == 0)
 * and every entry point reports ENOSYS. A partial install is never reported as
 * success, and a failed capture is fatal rather than a fabricated DEFAULT.
 *
 * The four engine signals are mandatory by nature. A captured NATIVE/GUEST
 * action is mandatory too, and is NOT silently degraded to "the kernel runs the
 * application's handler itself": that would bypass the one-shot consumption and
 * the logical mask while the getter kept reporting the logical snapshot, i.e. it
 * would report coverage this module does not have. Refusing startup (errno
 * EPERM) is the honest outcome. */
int box64dl_signal_actions_init(void);

/* Reverse of init. After the caller has quiesced deliveries (a caller
 * obligation), restore the application actions captured at init — but only where
 * the kernel disposition is still ours, so a host override installed after init
 * is left alone. A controlled NATIVE action installed *after* init also survives:
 * it is a live application action, not the captured pre-init one. After this
 * returns the module is inactive and every entry point reports ENOSYS until a
 * later init. */
void box64dl_signal_actions_fini(void);

/* 1 while init has succeeded and fini has not run. */
int box64dl_signal_actions_active(void);

/* The single application-decision point. The engine calls it after its own
 * hardware-fault repairs (or directly for a user-generated signal, and directly
 * for any hardware fault on a thread with no guest context) with `variant` =
 * which kernel entry was taken and `dynablock`/`x64pc` = the engine-computed guest
 * identity (NULL / (uintptr_t)-1 when not applicable). It never returns for a
 * DEFAULT action on a fatal signal; for every other outcome it returns to the
 * kernel entry, which returns from the signal frame.
 *
 * SYNTHETIC NULL ucontext. `native_uc` may be NULL: an engine-synthesized or
 * direct delivery (not one taken from a real kernel signal frame) may have no
 * real original host ucontext_t. A kernel-delivered signal, including a
 * user-generated one such as tgkill, always HAS a real ucontext_t and the engine
 * passes it through. When `native_uc` is NULL a NATIVE handler receives NULL for
 * that argument (still by SIGINFO arity when SA_SIGINFO is set), and the logical
 * mask is applied on top of the thread's CURRENT mask instead of an interrupted
 * uc_sigmask. No frame is fabricated.
 *
 * FAILURE IS TERMINAL, NEVER A SILENT FALLBACK. This entry point is void and has
 * no errno channel. If the bounded reader window cannot be established (the
 * reference is then not even taken) or cannot be restored, the module writes an
 * async-signal-safe diagnostic to stderr and terminates — for a fatal engine
 * signal through the irreversible terminal default, so signal identity, core
 * status and wait status stay faithful, and otherwise with exit status 127. It
 * never runs the application's action in an unguarded reader section and never
 * drops the delivery silently. This path is unreachable in a well-formed process,
 * because init() refuses startup with EPERM when the window primitives fail. */
void box64dl_signal_deliver(x64emu_t* emu, int host_sig, siginfo_t* info,
                            void* native_uc, int variant, void* dynablock,
                            uintptr_t x64pc);

/* The engine's 4-argument dispatch hook, defined by patches/0020 in
 * src/libtools/signals.c. The module's four static kernel entries reference it
 * (one definition, engine-owned). It runs the engine's own hardware-fault repairs
 * and then calls box64dl_signal_deliver(). A signal the hook classifies as
 * user-generated, and any hardware fault on a thread with no guest context, never
 * reaches the repairs. */
typedef void (*box64dl_engine_signal_hook_t)(int32_t host_sig, siginfo_t* info,
                                             void* ucntx, int variant);
extern box64dl_engine_signal_hook_t box64dl_engine_signal_hook;

/* Optional NATIVE-invocation seam (internal; not public ABI, and no observable
 * contract change). It is DEFINED by the module itself and defaults to NULL, so
 * a standalone build links with no extra unit and behaves exactly as before.
 *
 * When the engine sets it (once, before any delivery can occur), every NATIVE
 * application invocation goes through this wrapper instead of the module calling
 * the handler directly. The wrapper exists for ONE reason: a native handler may
 * be entered while a guest handler's TLS is active, and a foreign longjmp out of
 * that native handler would strand the guest bridge frame. The wrapper can
 * suspend/restore its own TLS around the user function so no TLS reference
 * outlives the call.
 *
 * The wrapper's obligations, so the two sides cannot disagree:
 *   - the application mask has ALREADY been applied by the module before this is
 *     called; the wrapper must NOT apply it again and must not touch the mask;
 *   - the module preserves errno across the invocation in both paths, so the
 *     wrapper need not;
 *   - it must invoke exactly the handler named by `action` (with SIGINFO arity
 *     when action->flags has SA_SIGINFO) and must not substitute or wrap it in
 *     any user-visible way;
 *   - it must leave no state behind that outlives the call, because a handler is
 *     allowed to depart by a foreign longjmp. */
typedef void (*box64dl_native_call_hook_t)(const box64dl_signal_action_t* action,
                                           int host_sig, siginfo_t* info,
                                           void* native_uc);
extern box64dl_native_call_hook_t box64dl_native_call_hook;

/* The guest worker, implemented by patches/0021 in src/libtools/signals.c: it
 * owns the guest-side lookup and the guest handler invocation (it may use the
 * engine's existing my_sigactionhandler_oldcode machinery, including fabricating
 * an emu when the guest needs one). It is called with no module lock and no module
 * reference held:
 *   - `action` is a caller-owned copy already taken by value; it stays valid for
 *     the whole call, and no module state may be read after it returns;
 *   - `emu`, `dynablock` and `x64pc` are the engine-computed values (emu is NULL
 *     on a thread with no guest context);
 *   - `native_uc` is the real original host ucontext_t of the delivery, or NULL
 *     for an engine-synthesized delivery (see box64dl_signal_deliver()).
 * A native bridge frame owns its TLS bookkeeping locally, so a guest longjmp
 * cannot leave a dangling frame behind. */
void box64dl_signal_guest_deliver(x64emu_t* emu, int host_sig, siginfo_t* info,
                                  void* native_uc,
                                  const box64dl_signal_action_t* action,
                                  void* dynablock, uintptr_t x64pc);

#ifdef __cplusplus
}
#endif

#endif /* LIBBOX64_SIGNAL_ACTIONS_H */
