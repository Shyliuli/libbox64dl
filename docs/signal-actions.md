# Process-wide signal actions (`include/libbox64_signal_actions.h`, `src/signal_actions.c`)

One **current application signal action per tracked host signal**, shared by the native
host and the guest, owned by the engine's dispatcher. The design is frozen; a
contradiction with it is a defect, not a variant. The normative contract lives in the
header and the sources' comments; this document records the design, the evidence and the
limits. What the source does and what has actually been exercised are kept apart.

Shipped revision — `src/signal_actions.c` `25d7156a`, `include/libbox64_signal_actions.h`
`9f28d3a1`, `src/signal_guest.c` `1021ac32`, `patches/0020` `95c7427c`,
`patches/0021` `bd75e3f6`. Per `AGENTS.md` all engine changes stay tree-out in `patches/`;
`vendor/box64` is never hand-edited.

## 1. What this replaces

The engine kept only the *guest's* view of a signal action (`my_context->signals[]`,
`is_sigaction[]`, `restorer[]`, `onstack[]`) and installed its own kernel handlers for the
engine signals. There was no host-side action record, and delivery was decided by **thread
context**: `src/signal_route.c` forwarded the four engine signals to the pre-box64 host
action only when `emu == NULL || emu->segs_offs[_GS] == 0`. That is a **GS** test, and a
plain Linux-x64 guest keeps its TLS in FS and leaves GS 0, so its dynarec-protected pages
were misrouted. `rt_sigaction` reached the kernel only through `my_sigaction` /
`my_syscall_rt_sigaction`; for the four engine signals the kernel call was skipped
entirely, so a query's `old` fields came back **zeroed**, and the handler placed into
`oldact` was `MY_SIGHANDLER` behind a `//TODO should wrap...` gap — an incomplete
native-bridge mapping.

**Frozen:** the discriminator is the **logical action kind**, never GS; there is **one
process-wide logical action per tracked host signal** that host and guest both read and
write; **last native or guest registration wins** — one shared slot, not per-side
isolation. `src/signal_route.c` is now a shim: `box64dl_signal_route()` returns 0, does no
classification, holds no state and forwards nothing, so the removed GS bypass cannot come
back through it.

## 2. Domain and vocabulary

| term | meaning |
| --- | --- |
| host signal | the native Linux signal number |
| engine signals | SIGSEGV, SIGBUS, SIGILL, SIGABRT |
| kind | `DEFAULT` / `IGNORE` / `NATIVE` / `GUEST` (`BOX64_SIGNAL_ACTION_*`) |
| variant | which of the four static kernel entries was taken |

Tracked domain is **1..31 and 34..64**. `host_sig` 32/33 are the C library's internal
pthread signals (SIGCANCEL/SIGSETXID): every entry point rejects them with **ENOTSUP and
never modifies host internals** — there is no raw `rt_sigaction`/`rt_sigprocmask` bypass
that could overwrite the C library's own handlers. `BOX64DL_SIGNAL_MAX` is 64 so a caller
can ask about 32/33 and get a documented refusal instead of a spurious out-of-range error.
`SIGKILL`/`SIGSTOP` are answered **DEFAULT** by the getter (legal — they can only ever be
DEFAULT); a **set on them is EINVAL**. The struct layout is fixed by the header and is not
restated here.

## 3. One current action, captured before engine init

`box64dl_signal_actions_init()` is called from the engine's `init_signal_helper()`
(patch 0020) — the first moment the engine holds a kernel action and the last moment the
application's own actions are still the process dispositions. That capture **is** the
pre-init snapshot, and a later fini restores it. `box64_signal_action_get()` reads the
same current action a guest `rt_sigaction` writes, and vice versa. A `get` never reports a
guest address as native and never reports the module's own kernel entry as the
application's handler.

## 4. Data model

```text
  pair { cur, reset }          one allocation unit, recycled as a unit
  slots[sig].latest            authoritative current node
  slots[sig].variant[v]        fast-path node for kernel entry variant v
  slots[sig].installed/flags   the kernel disposition we installed
```

- `_Atomic` is used for the slot **pointers**, the generation counter, the reader count
  and a node's `consumed` flag — never a per-field atomic struct. A reader copies a
  **whole snapshot by value** out of an **immutable** node through a published pointer. A
  node is filled while still off every slot (on the freelist) and is **never mutated once
  published**.
- **Fixed, preallocated pool of 512 `{cur, reset}` pairs**, consumed at init and recycled;
  nothing on a delivery path allocates or frees. The bound covers the number of
  **simultaneously rooted** nodes (per tracked signal: the capture node plus the nodes
  rooted by the four variant slots and `latest`), not the number of writes.
- `reset` is the preallocated one-shot successor of its `cur`; a recycled `cur` always
  already has its successor armed. Reclamation is by **pair**.
- **ENOMEM is a documented refusal, not a bug.** Only the *root capacity* is bounded; an
  unbounded retired-write rate is not claimed to succeed. A writer stream that never
  reaches a global quiescent moment (reader count 0) cannot have its retired pairs
  recycled and is refused with ENOMEM. There is **no grace waiting**: `reclaim_retired()`
  skips reclamation while the counter is non-zero and never spins or blocks; once readers
  drain to 0 the pool recovers.
- A retired pair is recycled only when the writer observes **reader count 0 and the pair
  is not rooted** by `latest`, by **any** variant slot, or as a `reset`. ABA safety is
  **conditional on that reader grace**, not asserted absolutely.

### 4.1 Reader grace period, and the getter's window

```text
Reader:  A: fetch_add(readers, 1, seq_cst)
         S: seq_cst loads of every slot pointer it will use
            … load / by-value copy / one-shot CAS …
         B: fetch_sub(readers, 1, seq_cst)
Writer:  W: seq_cst clearing stores to every slot rooting the node
         F: atomic_thread_fence(seq_cst)
         L: load(readers, seq_cst)
```

If `L == 0` no reader is between A and B: a reader that read the node at S read it before
the SC store that removed it (`S < W < F < L`); had its B been after L the counter would
still count it at L, contradicting `L == 0`, so `B < L` and the reader's whole use
completed before anything is recycled. Relaxed fences do not establish this; the counter
RMWs, the slot loads and the slot-clearing stores are all seq_cst. The reader's critical
section **includes the load, the by-value copy and the one-shot CAS**, and `reader_exit()`
runs **before any application callback**, so a handler leaving its frame by a foreign
`longjmp` strands no reference and no latch. There is no per-thread handler-depth latch and
no refusal state that outlives a call.

The delivery's own reference is not the whole story: a public `box64_signal_action_get()`
interrupted by a foreign jump would abandon its reference inside the section, leaving
`g_readers` permanently non-zero and draining the pool. The public getter therefore
brackets the section with a **signal-mask window**: all catchable signals are deferred on
the calling thread, the reference is taken, the snapshot copied, the reference released,
and the caller's mask restored **after** the release and **before** any pending callback
is delivered. No user mutex and no allocation on that path; if the window cannot be
established the call refuses with EPERM rather than run an unbounded section. The delivery
path uses the same window and treats a failure as terminal (never a silent unmasked
fallback). Asynchronous cancellation and thread exit are **outside** this contract — they
can unwind a thread out of the section and are documented as such in the header.

### 4.2 Variant slots, and why the entries are static

The kernel, not the module, applies `SA_ONSTACK`/`SA_RESTART` at delivery time and does
not tell the handler which flags it runs under, so the entry point itself must encode the
variant — the 2-bit encoding of those two logical flags:

| variant | SA_ONSTACK | SA_RESTART | macro |
| --- | --- | --- | --- |
| 0 | no | no | *(plain; no macro)* |
| 1 | yes | no | `BOX64DL_SIGNAL_VARIANT_ONSTACK` |
| 2 | no | yes | `BOX64DL_SIGNAL_VARIANT_RESTART` |
| 3 | yes | yes | `BOX64DL_SIGNAL_VARIANT_MASK` |

`BOX64DL_SIGNAL_VARIANT_MASK` is **3**, the both-bits case — not "no flags";
`BOX64DL_SIGNAL_VARIANTS` is 4. The four variants are four **static C functions** with an
`SA_SIGINFO`-shaped body, each passing its own constant. No dynamic machine thunk;
`sa_restorer` is never installed as the kernel restorer.

## 5. Publication order and failure

A set plans the kernel action, then, holding the writer mutex:

1. pops a `{cur, reset}` pair and fully fills both nodes, **reserving the candidate and
   reset generation IDs from the per-signal counter before publishing**, so a later
   failure leaves a **numbering gap** but never a duplicate;
2. **publishes the target variant slot**, retaining all other variant roots — each roots a
   node of its own variant and clearing one would strip a node a delivery may still be
   reading;
3. **installs the kernel disposition** if `kernel_current()` says the entry or the kernel
   flags actually changed;
4. **exchanges `latest`** only after step 3 succeeded.

`kernel_current()` is authoritative by **real query**, not by the cached record: it
re-reads the live disposition and compares the actual entry pointer and the **full** kernel
`sa_flags`. That matters because the module's own default-stop path (§9.1) leaves the
kernel at raw `SIG_DFL` behind a stale cache entry.

**Failed publication.** If the kernel install fails, `latest` is **untouched**, so the old
authoritative action stays current and a later `get()` reports what is actually installed.
The target variant slot is restored by **CAS only if it still holds the failed candidate**.
The candidate pair is **retired, not freed immediately** — a concurrent delivery may have
loaded it before the restore. **`*old` on failure is not promised**: Linux leaves `oldact`
undefined there, so the module does not bind itself to filling it, and no test may assert
an `old` value on a failed set.

## 6. Writer serialization, and what is not async-signal-safe

Writers are serialized by a mutex the module only ever **tries** to take (`trylock`), with
every catchable signal blocked on the writer's own thread for the duration and the saved
mask restored afterwards. A failed try returns **EWOULDBLOCK before any state is touched**.
A `pthread_mutex_t` is not async-signal-safe, which is exactly why it is try-only.

**`box64_signal_action_set_native()` and a setter `act == NULL` query are NOT POSIX
async-signal-safe.** The **getter is public, lock-free and bounded**: no lock, no
allocation, no application callback; its only non-computation work is the `pthread_sigmask`
pair of §4.1. The engine's own emulation and repair path is likewise not async-signal-safe,
and no claim is made otherwise.

## 7. Kernel action and logical flags

For signal `s` with logical action `a`:

```text
sa_mask  = EMPTY                       (sigemptyset)
sa_flags = SA_SIGINFO                  (forced: the entries are SIGINFO-shaped)
         | SA_ONSTACK   if (a.flags & SA_ONSTACK)
         | SA_RESTART   if (a.flags & SA_RESTART)
         | SA_NOCLDWAIT / SA_NOCLDSTOP  if s == SIGCHLD
```

There is **no `SA_NODEFER` and no `SA_RESETHAND`** in the kernel action: the kernel's
automatic self-block is relied on, and the one-shot reset is implemented on the immutable
snapshot (§10) so a concurrent replacement can never be erased by a kernel reset. The
**logical** `flags` field is exactly what the application asked for and is never rewritten
to the kernel's forced set. `flags` is a `uint64_t` holding an *unsigned* 32-bit `sa_flags`:
every producer widens through `(uint64_t)(uint32_t)sa_flags` and every consumer compares
the low 32 bits, because `SA_RESETHAND` is `0x80000000` and a signed widening would break
both comparison and kernel reconciliation.

A **true DFL/IGN** action on a non-fatal signal is installed as the kernel's real
`SIG_DFL`/`SIG_IGN` with the application's mask and flags (SA_SIGINFO dropped, since such a
disposition cannot name a SIGINFO handler). SIGCHLD's NOCLDWAIT/NOCLDSTOP are **kernel**
semantics and propagate even while the dispatcher owns the handler, including in the IGN
case.

## 8. Mask semantics

The mask the **application handler** observes is computed from the application context
only:

```text
want = uc_sigmask(native_uc)   (the pre-delivery mask the kernel recorded;
                                the current thread mask if there is no uc)
     | a.mask                  (the application's sa_mask)
     | {host_sig}  unless (a.flags & SA_NODEFER)
sigprocmask(SIG_SETMASK, &want, NULL)
```

Two traps this avoids: the self bit is **not** cleared under `SA_NODEFER` (an application
may put it in its own `sa_mask`, and that explicit bit must stay), and the mask is **not**
reconstructed from the kernel mask, which is deliberately empty.

**Edits across the resume.** An ordinary handler `sigprocmask` addition is an **extra** and
is cleared on return; only edits the handler made to the delivered `ucontext`'s `uc_sigmask`
survive the resume, and pre-existing bits survive unless the `uc` explicitly clears them.
Identical under FAST and COMPAT, including the changed-`mcontext` engine jump, which goes
through the existing pending-mask hook. For `SA_RESTORER` the application's restorer
address survives a guest query → reinstall → query round trip, is **never installed as a
kernel restorer** and never executed natively.

## 9. Delivery: repair first, then the logical kind

`box64dl_dispatch_entry()` is the body shared by the four static entries:

1. **Classify.** A user-generated event (`si_code <= 0`) needs no fault maintenance and
   goes straight to the application decision. A hardware fault on one of the four engine
   signals goes through `box64dl_engine_signal_hook`. **Neither is keyed on GS.**
2. **Internal repair first** — `protectDB`/auto-SMC, `sigbus_specialcases`, the
   `unlockMutex(Locks)`/`relockMutex(Locks)` bracketing — **including the GS=0 case**,
   which is the whole point: a plain Linux-x64 guest's dynarec write-protect fault must be
   repaired even with GS base 0.
3. **Route the application decision** (the logical kind) and run the engine's own repair
   before the callback. The decision sits **outside `#ifdef DYNAREC`** and after every
   repair check, so a build without the dynarec still reaches the module.
4. **Diagnostics/logging are not maintenance** and may not precede a native or guest user
   handler.

The dynarec mutexes are released before the decision and **stay released across it**: an
application callback must not inherit the engine's locks, and a foreign `longjmp` out of it
must not strand them. Only a normal return re-takes them. The upstream repair itself uses a
mutex and an rbtree and is **not** async-signal-safe; the module claims boundedness and
re-established lock discipline, not a safety proof.

| kind | action |
| --- | --- |
| `DEFAULT`, engine signal | terminal default (§11) — does not return |
| `DEFAULT`, non-engine | true kernel `SIG_DFL`: terminate, default-ignore (SIGCHLD/SIGCONT/SIGURG/SIGWINCH) or default-stop (SIGTSTP/SIGTTIN/SIGTTOU) |
| `IGNORE` | return; the kernel disposition is true `SIG_IGN` |
| `NATIVE` | call the native C handler with the **real** `siginfo_t`/`ucontext_t`, by `SA_SIGINFO` arity (3 args) or 1 arg |
| `GUEST` | hand to `box64dl_signal_guest_deliver()` (§12) |

If the module is inactive while its dispatcher is still the kernel disposition (between
fini and a re-init), an engine fatal signal takes the terminal path rather than returning
into a frame that would re-execute a fatal fault forever.

### 9.1 Default-stop

A default-stop signal is raised with a **raw `SIG_DFL`** and the process continues after
`SIGCONT`. That raw install changes the real kernel state behind the cached record — which
is exactly why §5's `kernel_current()` does a real query.

## 10. One-shot (`SA_RESETHAND`)

RESETHAND is virtual: the kernel action carries no reset bit and the dispatcher stays
installed. Consumption is a **single CAS on the node** (`n->consumed`), under the reader
reference and before any callback, so it fires exactly once per generation even if `latest`
has already been replaced, and even if several deliveries loaded the same node (exactly one
wins).

- A **CAS loser** delivers **this node's own reset snapshot** — the preallocated logical
  DEFAULT successor — and **never** whatever newer action `latest` happens to hold. That is
  precisely how one handler would otherwise fire twice.
- The winner's install of the reset is a **conditional CAS on `latest`** that still names
  this node, so it can never overwrite a newer registration.
- A setter whose staged node was already consumed before its own exchange performs the
  equivalent reconciliation after the exchange, or the one-shot reset is lost.
- `generation` was reserved when the pair was armed; the reset publication carries a
  strictly newer unique number.

## 11. Fatal `DEFAULT`

For an engine signal, `terminal_default()` installs kernel `SIG_DFL`, **unblocks self** (the
kernel's automatic self-block would otherwise leave the re-raise pending forever),
`tgkill`s the process on its **own TID**, and never returns. It does not reinstall the
dispatcher and the process does not resume; signal identity, `WIFSIGNALED`, core status and
exit status are the kernel's own. It is **not** "temporarily restore the host action and
forward": the pre-init host action is not re-installed.

## 12. Context boundary, guest worker, bridges

- **NATIVE receives the real native context** — the actual `siginfo_t` and the real host
  `ucontext_t` the kernel built. Nothing is fabricated for it. When `native_uc` is NULL (an
  engine-synthesized or direct delivery, never a kernel-delivered one) a NATIVE handler
  receives NULL for that argument, still by `SA_SIGINFO` arity when the flag is set, and the
  logical mask is applied on top of the thread's current mask. No frame is fabricated.
- A guest handler needs a resumable x86 context. A genuine fault `ucontext_t` cannot be
  turned into a resumable x86 PC, and the engine must never **discard an interrupted native
  C frame** by driving an engine `longjmp` from a GUEST `uc`-edit. So a guest handler runs
  on an **emulated** guest context — the `uc` is **not** a byte copy of a native kernel
  `uc` — with the real `native_uc` passed along as evidence. A native C fault taken while
  the logical action is a GUEST handler is repaired either by the guest handler's
  `mprotect`-and-resume or by **chaining to the native bridge with the real context**; both
  directions are exercised by the `native-c-fault` case.
- **Both stack domains and both flag behaviours are honoured, and are verified.** The
  guest's own **virtual altstack** (the `my_sigaltstack`/`sigstack_key` machinery) and the
  **real native kernel altstack** are independent domains: a GUEST `SA_ONSTACK` action lands
  on the guest's virtual stack, a NATIVE `SA_ONSTACK` action lands on the real kernel
  altstack, and neither replaces the other. `SA_RESTART` is likewise honoured for a NATIVE
  action against a **real** host syscall. Cases `altstack` and `restart` assert exactly
  that. What remains true and is not a weakening: the guest runtime does not reproduce the
  kernel's byte-level frame layout — the kernel-selected variant is guaranteed to be the
  variant whose snapshot the reader copied.
- **`rt_sigaction(sig, NULL, &old)`** returns the **original guest address** for a guest
  action and a **stable, guest-ABI-callable native bridge** for a native one (a host handler
  cannot be represented as a guest address). A guest query round trip preserves the kind,
  including NATIVE.
- The guest worker `box64dl_signal_guest_deliver()` is engine-owned (patch 0021). It reads
  the guest **address** from the by-value snapshot, builds the guest frame from the Box64
  x86 state, converts the mask to the guest ABI (`x64_sigset_t`), and preserves
  `sa_restorer` without executing it natively. Guest frames carry an epoch and are
  invalidated on any resume jump; before calling any application handler whose return is not
  guaranteed, the TLS frame array is copied to a local, cleared and the epoch bumped, so a
  foreign `longjmp` leaves the TLS empty instead of dangling.
- **Native bridge cache is local, keyed by function + arity**, because calling
  `AddCheckBridge()` unlocked against the engine's khash is a race. The bridge path uses a
  **local cache + `AddBridge`** under one lock, and overflow is an **ENOMEM**, not a silent
  fallback.
- **Module-owned optional native-call hook** (`box64dl_native_call_hook`): a typedef taking
  `(const action, int host_sig, siginfo_t*, ucontext_t*)`, defined by the module and
  defaulting to NULL, so a standalone build needs no third-unit stub. The module has
  **already applied the mask** before the hook, so the hook must not re-apply it and must not
  touch the mask. The engine sets it at init and clears it at a quiesced fini.
- **Guest module lifecycle:** `box64dl_signal_guest_reset()` clears frames, the pending
  resume mask and the bridge cache **before** the bridge is freed; the caller must have
  quiesced deliveries.

## 13. Lifecycle

- `init()` is **transactional**: 0 only when every tracked action was captured **and** every
  required dispatcher installed — the four mandatory engine signals, plus every tracked
  signal whose captured action is NATIVE or GUEST. Any failure rolls every partial install
  back to the exact captured pre-init action, leaves the module inactive (`active() == 0`,
  every entry point ENOSYS) and reports EPERM. A capture failure is fatal: a DEFAULT is
  never fabricated for an action it failed to capture, and a captured NATIVE/GUEST action is
  never silently degraded to "the kernel runs it itself". Init also refuses with EPERM when
  the reader window primitives or the lock-free atomics it depends on are unavailable,
  leaving the process untouched.
- `fini()` is legal **only** with all guest engine work and deliveries quiesced — a caller
  obligation. It restores the pre-init actions only where the kernel disposition is **still
  ours**, so a host override or a controlled native action installed after init survives; a
  guest action falls back to the captured pre-init action, since a guest address cannot
  outlive the engine.
- **Teardown ordering** (patch 0020): `fini_signal_helper()` was moved out of
  `finiAllHelpers()` into the early part of `FreeBox64Context()` — after the ctx-local
  teardown but **before `my_context` is nulled and before `FreeBridge`** — and the obsolete
  raw `ctx->signals[]` reset loop over tracked actions was removed. Both were too late in
  upstream order, which would have torn the signals down after the ELF/bridge/resources.
- **A raw host `sigaction` override installed after init is unsupported.** The supported
  native surface is `box64_signal_action_set_native()`; the supported guest surface is
  `rt_sigaction` through the engine path.

## 14. Evidence

| artifact | revision | state |
| --- | --- | --- |
| `src/signal_actions.c`, `include/libbox64_signal_actions.h` | `25d7156a`, `9f28d3a1` | shipped; unit 15/15 inside the current full suite (§14.1) |
| `src/signal_guest.c` | `1021ac32` | shipped; component evidence plus the real-engine matrix (§14.2) |
| `patches/0020`, `patches/0021` | `95c7427c`, `bd75e3f6` | applied and verified in a live cycle; the engine is built from this queue (§14.3) |
| `tests/` driver, guest fixture, case list | current revision | **76/76 processes PASS** on the real engine; the 22-case list is the source of truth (§15) |
| getter-interruption probe | finding `2be4654c`, repair `25d7156a` | `build/signal-actions-interrupted-getter-probe.log` and `...-regression-leader-final.log` (§4.1) |

### 14.1 Unit evidence (current revision)

`make test` rebuilds and runs the standalone `test-signal-action-unit` against the current
`src/signal_actions.c`, ending at `=== 15/15 cases passed ===` in
`build/signal-actions-full-test-cleanup-leader.log`. The target links `src/signal_actions.c`
with two symbol doubles and `-Wl,--wrap=sigaction`, driving the **real module entry
points**. The 15 cases: `init_capture`, `four_variants`, `range_and_reserved`,
`chld_kernel_flags`, `injected_set_failure`, `injected_init_failure`,
`staged_oneshot_before_latest`, `old_entry_variant`, `pool_20k`, `foreign_longjmp`,
`uc_mask_edit`, `multithread_snapshot`, `guest_route`, `default_fatal`,
`enomem_boundary_and_recovery`.

Covered there, and not to be described as untested: **fault injection** (the two
`injected_*` cases), a **real post-kernel / pre-`latest` window**
(`staged_oneshot_before_latest`), the four variants, mask handling, a 20 000-cycle pool run,
multithreaded coherence with the ENOMEM boundary and recovery, and **`foreign_longjmp`**.

Honest limits: the unit does not cover the engine or the guest ABI — the engine hook is a
stub there, the four static kernel entries are not addressable from a standalone test, fini
is exercised only on a quiesced table, and there is no guest frame construction or x86_64
mask conversion. Under 5-way contention most setter attempts are refused with the documented
EWOULDBLOCK; that is the try-only writer mutex behaving as specified, a throughput property
rather than a contract breach.

### 14.2 Guest worker

`src/signal_guest.c` is a freestanding-safe translation unit compiled into the grafted
engine. Its component evidence is `build/signal-actions-guest-component-leader.log`: V1
plain `-std=c11 -Wall -Wextra -Werror` exit 0 and V2b the exact engine flag set plus the
same warnings exit 0, with 244 checks / 0 failures, using only three engine doubles
(`my_context`, `AddBridge`, `GetNativeFnc`). That log pins the guest ABI conversion in
isolation; it is not engine evidence, and the same source is what the real-engine matrix
below drives.

### 14.3 Real engine: queue cycle, build, matrix, full suite

- **Live queue cycle** (`build/signal-actions-live-queue-leader.log`): `make patch` →
  `make unpatch` → `make patch` → `tools/verify_box64_patch.sh --check`. The reverse walked
  `0021..0001`, the re-apply restored the tree, and the verifier re-applied the queue to a
  pristine export and compared the affected files with the live tree directly — **no drift**,
  the feature symbols absent after the reverse and present after the re-apply.
- **Build** (`build/signal-actions-build-stage-leader.log`): `make static` plus the Rust
  static build from that queue, exit 0, with `src/signal_actions.c` and `src/signal_guest.c`
  compiled into the grafted engine.
- **Matrix** (`build/signal-actions-matrix-cleanup-leader.log`): `make test-signal-actions`
  runs every row of `tests/signal-actions-cases.list` in its declared policy and dynarec
  mode, one fresh process each: `== signal-actions: 76 process(es) passed ==`, 76 PASS /
  0 FAIL.
- **Full suite** (`build/signal-actions-full-test-cleanup-leader.log`): `make test`, exit 0,
  with every legacy target, the unit above, the matrix, the DSO/shared linkage and the
  consumer DSO guest (including its destructor at fini). The one `[BOX64] Error:` line is the
  bridge-wiring gate's deliberate fail-closed probe; its following check passes and it is not
  a failure result.

The matrix had not been run end to end before this work, and reaching each new row for the
first time surfaced eight defects — **all of them in the test layer** (drivers, fixtures and
expectation values), none requiring a production change, and none resolved by removing or
weakening an assertion.

## 15. Test matrix

`tests/signal-actions-cases.list` is normative: **22 distinct cases** in **39 data rows**,
expanded by the `test-signal-actions` recipe to **76 processes**. Cases: `alive`,
`host-preinit`, `guest-replace`, `native-override`, `unity`, `roundtrip`, `not-init`,
`invalid`, `uc-mask`, `longjmp`, `resethand-race`, `flags-mask`, `altstack`, `restart`,
`resethand`, `smc`, `pagefix`, `native-fault`, `native-c-fault`, `chld`, `concurrent`,
`fatal`. Fields 1–3 of a non-indented line are `case policy modes`; indented lines are
wrapped prose. A `-` policy column leaves `BOX64DL_SIGNAL_POLICY` unset, so the engine
default is what the case exercises.

`resethand-race` and `concurrent` deliberately run on **SIGWINCH (DEFAULT-IGNORE)** so an
arbitrarily timed second delivery during the race is a legal, discarded default delivery
instead of a legal fatal exit that would misreport the test as an engine failure.
`EWOULDBLOCK` and `ENOMEM` are allowed bounded refusals during the race; after it quiesces a
**new registration must succeed** and run its own handler exactly once. The
allocation-sensitive cases run under `MALLOC_PERTURB_`.

## 16. Limits and what is not claimed

- **Not async-signal-safe end to end.** The getter and the delivery's snapshot are lock-free
  and bounded; the native setter and a setter NULL-query are explicitly not
  async-signal-safe, and the upstream repair path performs operations that are not.
  Asynchronous cancellation and thread exit can unwind a thread out of a reader section.
- **No `1..64` fidelity**: the domain is `1..31` and `34..64`; 32/33 are ENOTSUP.
- **A raw, uncontrolled native `sigaction()` replacement on a tracked signal after init is
  unsupported.**
- **`fini()` requires caller-quiesced deliveries**; only that path is supported.
- **A failed kernel install does not promise `*old`** and leaves the previous action in
  force. A guest-side export failure (bounded bridge cache) is reported as a refusal and
  never degrades to a different action.
- **The guest `ucontext_t` is not a byte copy of a native kernel `ucontext_t`.** The two
  stack domains are independent; the `SA_ONSTACK` and `SA_RESTART` **behaviour** each side
  observes is the verified contract (cases `altstack`, `restart`).
- **Coverage gaps, backlog only**: `SA_NOCLDSTOP` without `NOCLDWAIT`, `SI_TIMER`
  classification, and TSan/helgrind over the grace period are not covered by the current
  matrix.
