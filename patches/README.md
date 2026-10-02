# vendor/box64 patch queue

The engine is the **upstream** [box64](https://github.com/ptitSeb/box64)
repository, checked out as the git submodule `vendor/box64` at tag `v0.4.2`
(commit `7eeb5016493dab4e143d53da50dd47bfb44a9509`), plus the ordered patches in
this directory. There is no local vendor fork and no hand-edited copy of box64 in
this repository: the submodule stays re-checkoutable, `make patch` applies the
queue into its worktree and `make graft` links this repository's sources into it.

- `0001..0019-*.patch` — the base queue, applied in filename order by
  `make patch` and reversed by `make unpatch`.
- `0020-signal-action-engine.patch`, `0021-signal-guest-actions.patch` — the
  signal-action engine half; applied and verified in a real live cycle, and the
  engine built from that queue (see the section below).
- `../tools/verify_box64_patch.sh` — verifier: it re-applies the whole queue to a
  pristine export of the pinned submodule commit and compares the affected files
  directly with the live worktree. No file hashes or lock are kept: the
  comparison is byte-for-byte and needs no second copy of the truth.

```sh
tools/verify_box64_patch.sh --check   # submodule commit + pristine apply + worktree drift
make patch                            # apply the queue to vendor/box64
```

## What each patch is

| patch | scope |
| --- | --- |
| `0001-export-openFTrace.patch` | export `openFTrace` for the Rust bootstrap |
| `0002-silent-log-default-unless-env.patch` | default-silent logging unless the env asks |
| `0003-signal-host-route.patch` | route host-context faults away from guest SEH; add `thread_peek_emu` |
| `0004-known-abi-dynacall.patch` | `DynaCall_abi` known-ABI entry |
| `0005-signal-jmpbuf-policy.patch` | per-frame signal-mask policy |
| `0006-fini-restore-host-actions.patch` | fini restores pre-box64 host signal actions |
| `0007-bridge-dedup-key.patch` | bridge dedup key + fully initialised slots |
| `0008-serialize-mapping-null-guard.patch` | `SerializeAllMapping()` tolerates an empty mapping table |
| `0009-embedded-endbox64-empty-context.patch` | `endBox64()` tolerates an embedded context with no main ELF |
| `0010-native-module-seam.patch` | engine side of `box64_register_native_module()` in `initWrappedLib()` |
| `0011-embed-libbox64-static-target.patch` | CMake `libbox64` STATIC target, `BOX64_MAIN_SRC` split, generated include lists, `-rdynamic` |
| `0012-plugin-library-list-hook.patch` | `library_list.h` includes the generated plugin table |
| `0013-x87-tagword-fxsave-fidelity.patch` | logical/physical x87 tag word, FXSAVE/FXRSTOR register image, `BOX64_MXCSR_MASK` |
| `0014-la64-fpu-flags-and-16b-atomics.patch` | LA64 dynarec FSQRT C0 and unaligned 16-bit LOCK atomics |
| `0015-la64-next-linker-frame-sentinel.patch` | mark the live `LinkNext` host-stack frame |
| `0016-printer-plugin-signatures.patch` | `x64printer` cases for the seven plugin seam signatures |
| `0017-interpreter-fpu-exception-flags.patch` | interpreter FSQRT sets the x87 C0 flag |
| `0018-compile-portability-includes.patch` | `signals.h` / `dynablock.c` / `rbtree.c` include fixes |
| `0019-cleanup-thread-safety.patch` | serialise the atexit / thread-local destructor tables; publish capacity after `box_realloc()` |

Patches `0001..0010` are the embedded-engine/toolkit changes; `0011..0019` are
build and behaviour fixes against pristine upstream. Each header states its
rationale.

## Queue entries 0020 / 0021 (applied, verified, built)

`0020` and `0021` are the engine half of the process-wide signal actions
(`include/libbox64_signal_actions.h`, `src/signal_actions.c`). The two files now
exist in this directory:

| patch | scope |
| --- | --- |
| `0020-signal-action-engine.patch` | `src/libtools/signals.c` + `src/box64context.c`: `init_signal_helper()` publishes the seam and calls `box64dl_signal_actions_init()` — transactional, all four engine signals or a full rollback — and installs no legacy kernel handler afterwards; `box64dl_engine_signal_hook` is defined as the engine's 4-argument repair handler (`my_box64signalhandler` becomes a thin compatibility wrapper over the same body with the "current variant" sentinel); every genuine repair stays FIRST and **includes the GS=0 dynarec write-protect / auto-SMC case**; `box64dl_signal_deliver()` takes the application decision only after the repairs, with the dynarec mutexes released and re-taken around it; patches/0003's first-line `box64dl_signal_route()` call is removed, and with it the GS/TEB discrimination; `init_signal_helper()` also publishes `box64dl_native_call_hook`; `fini_signal_helper()` withdraws both seams, calls `box64dl_signal_actions_fini()` (restores only dispositions still its own) and `box64dl_signal_guest_reset()`, replacing patches/0006's per-signal restore; in `box64context.c` `fini_signal_helper()` moves out of `finiAllHelpers()` into `FreeBox64Context()` (before `my_context` is nulled and before `FreeBridge`) and the raw `signal(i, SIG_DFL)` loop over `ctx->signals[]` is removed |
| `0021-signal-guest-actions.patch` | `src/libtools/signals.c` + `src/os/emit_signals_linux.c` + `src/dynarec/dynarec.c`, consumed with `src/signal_guest.c`: every registration entry point (`my_signal`, `__sysv_signal`/`sysv_signal`, `my_sigaction`/`__sigaction`, `my_syscall_rt_sigaction`, `my_sigset`) becomes a view of the ONE logical action table — a guest query returns the ORIGINAL value (the guest pointer for a GUEST action, a stable guest-callable bridge for a NATIVE one, SIG_DFL/SIG_IGN otherwise), and a reinstall of that bridge lands back on the NATIVE action. Invalid signals keep Linux semantics, and the two classes differ: **32/33 are `ENOTSUP`/`SIG_ERR` on every path** (the C library's internal pthread signals are never touched), while **`SIGKILL`/`SIGSTOP` are `EINVAL` on a SET but a legal QUERY that reports the unchangeable DEFAULT** — the check lives on the set path only. `my_sigactionhandler_oldcode(_64)` takes the immutable action snapshot by value and fills the synthetic guest `uc_sigmask` from the delivering native context; a guest `uc_sigmask` edit is honoured on both the ordinary return and across a `siglongjmp` (the latter recorded for patch 0005's resume hook); `os/emit_signals_linux.c` routes emulator-generated signals through the same table; one `dynarec.c` hunk applies the recorded mask edit and drops abandoned guest frames in the `SigSetJmp` resume branch. It does **not** touch init/fini, the kernel entry variants or `box64context.c` |

**Status: applied, verified and built.** A real live cycle — `make patch` →
`make unpatch` → `make patch` → `tools/verify_box64_patch.sh --check` — ran against
the vendored tree with **zero drift** against the pristine upstream export at every
step (the unpatch output showed the queue being reversed in order, the re-apply
restored it, and the verifier re-applied the queue to the pristine export and
compared the files). The new engine was then built from that queue (`make static` +
the Rust static build) and the real-engine test matrix runs on that binary. Evidence:
`build/signal-actions-live-queue-leader.log` and
`build/signal-actions-build-stage-leader.log`.

**Seam closure.** The module `src/signal_actions.c` needs exactly two engine-half
symbols: `box64dl_signal_guest_deliver()` for a GUEST delivery and the
`box64dl_engine_signal_hook` pointer, both declared in
`include/libbox64_signal_actions.h`. `patches/0020-signal-action-engine.patch`
defines `box64dl_engine_signal_hook` (via `box64dl_engine_signal_hook_impl`) and
`patches/0021-signal-guest-actions.patch` defines `box64dl_signal_guest_deliver`
in `src/libtools/signals.c` with a signature byte-identical to the header
declaration. That is not only an artifact-level claim any more: the grafted
shared object links with `-Wl,-z,defs`, and the engine build in the live cycle
above is what would have failed on a missing definition. The internal half
(`box64dl_signal_action_get`, `box64dl_signal_actions_init`,
`box64dl_signal_guest_deliver`, `box64dl_engine_signal_hook`) is still rejected
from the public ABI by `tools/check_exports.py`.

Two boundaries that are **design restrictions**, so the plan is not overstated:

- **A native signal handler is a real native C function** and never runs on an
  emulated context; a **guest** handler address is never called as a native C
  function and is dispatched only through `src/signal_guest.c`. The two
  `ucontext` representations are not interchangeable, and the **guest virtual
  altstack** and the **real native kernel altstack** are independent domains
  (both honoured).
- **A direct, uncontrolled native `sigaction()` replacement on a tracked signal
  after init is unsupported.** The supported native surface is
  `box64_signal_action_set_native()`; the supported guest surface is
  `rt_sigaction` through the engine path.

With `0020`/`0021` and both sources present the Makefile gate is **open**
(exactly one `0020*.patch`, exactly one `0021*.patch`, both sources):
`src/signal_actions.c` and `src/signal_guest.c` become `ELFLOADER_SRC`, the public
`box64_signal_action_get` / `box64_signal_action_set_native` entries must appear
as dynamic symbols (enforced by `tools/check_exports.py` via
`--expect-signal-actions`), and `test-signal-actions` joins the default
`make test`. The gate's negative half rejects the module's internal half
(`box64dl_signal_action_get`, `box64dl_signal_actions_init`,
`box64dl_signal_guest_deliver`, `box64dl_engine_signal_hook`) from the ABI.

Build residue — `*.orig` patch pre-images, `build/`, `build-pic/`,
`rust/target/`, `src/git_head.h` and the patch stamp — is ignored by
`.gitmodules` (`ignore = untracked`) and never tracked. The generated wrapper
heads under `src/wrapped/generated/` are produced by box64's own
`rebuild_wrappers.py` from the grafted `*_private.h` files, so they are
regenerated rather than patched.
