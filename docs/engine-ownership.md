# Engine phase, ownership, and the foreign-jump boundary

This is the E1 ownership record for libbox64dl. It states what Rust owns, what C
owns, and which properties are structural (enforced by the design) versus merely
observed.

## 1. One phase owner, C-driven transitions

The engine phase lives in Rust (`rust/src/phase.rs`) as an explicit enum and is
published in one `AtomicU8` that only Rust writes:

```text
UNINIT --begin_init--> INITIALIZING --commit_init--> READY
                                     \--fail_init--> FAILED (terminal)
READY  --begin_close--> CLOSING --commit_close--> FINAL (terminal)
```

- `READY` carries the only owning handle (`Engine`); `INITIALIZING`, `CLOSING`,
  `FAILED` and `FINAL` carry none. The handle is dropped in `begin_close`, so no
  stale owning pointer can outlive the transition.
- C never keeps a phase copy. `box64_is_ready()` / `box64_engine_phase()` read
  the Rust-published word through the `box64dl_rust_phase_*` leaves.
- The Rust exports are **short leaves**: each takes the phase lock, mutates the
  state, publishes the phase and returns. They never call into the engine.
- C (`src/libbox64.c`) owns the orchestration and the engine calls:

  ```text
  box64_lib_init:   begin_init -> engine_boot -> commit_init | fail_init
  box64_lib_fini:   begin_close -> quiesce? endBox64 -> commit_close
  box64_ensure_emu: admit -> (create emu) -> release
  ```

  Because the guest-capable C work runs in a C frame with no Rust frame or
  guard above it, a foreign `siglongjmp` cannot skip a live Rust frame. This is
  the point of the split, not an accident of "Rust has no Drop here".

`my_context` is box64's own global. It is never a state source; C reads it only
after the phase admission succeeds.

## 2. Admission gate (C-owned, explicit finish)

`src/libbox64.c` keeps a plain C counter guarded by a C mutex:

```text
engine_admit()   -> refuse unless READY and not closing; ++inflight
engine_release() -> --inflight (explicit finish)
engine_quiesce() -> set closing; report whether inflight != 0
```

- Guest-capable entries (`box64_call_desc`/`box64_call_simple` through
  `call_core`, `box64_dlopen`, `box64_dlsym`, `box64_dlclose`,
  `box64_ensure_emu`, `box64_callback_with`, `box64_declare_host_mapping`)
  bracket their engine work with `engine_admit()` / `engine_release()`.
- There is deliberately **no Rust RAII ticket** spanning the guest: a foreign
  jump could skip a `Drop`. The release is an explicit C call, and the engine's
  own `siglongjmp` target is inside `DynaCall`, so a signal-driven resume still
  reaches it. A host `longjmp` that abandons the frame leaves the count raised
  on purpose: `box64_lib_fini()` then refuses teardown instead of force-freeing a
  live context.

## 3. Shutdown is terminal and fail-safe

`box64_lib_fini()` publishes `CLOSING` **before** any teardown, then:

- if calls are still in flight, it publishes `FINAL` and returns with a clear
  error, leaving the context allocated. The in-flight call completes normally;
  the engine is unusable and the process must exit;
- otherwise it calls box64's `endBox64()` (guest fini/destructors) in a C frame
  and publishes `FINAL`.

Every public entry checks the phase before touching the engine, including the
`call_entry` cached-emu fast path (readiness is checked **before** the cached
emu is returned), so no entry runs on a half-torn engine. `box64_lib_fini()` is
a no-op unless the phase was `READY`.

## 4. Why a double free is structurally impossible

The context memory is freed only by the C shim, on two mutually exclusive routes:

1. `box64dl_core_end()` delegates to box64's own `endBox64()`, which calls
   `FreeBox64Context(&my_context)` exactly once and sets `my_context = NULL`.
   `endBox64()` has an early-return path (emu workers still alive) that does
   **not** free; the shim reports which happened (return 1 = freed, 0 = not).
2. `box64dl_core_free_context()` calls `FreeBox64Context(&my_context)` once, and
   is used **only** when initialization failed after the context was created and
   the full teardown must not run.

Rust never calls a free itself and `Engine` has no freeing `Drop`. Therefore a
Rust destructor cannot race box64's teardown.

## 5. What stays in C, and why

The private shim (`src/core_shim.c`) is mechanical only: it moves values in and
out of box64's private structures and calls functions Rust cannot call directly.
The low-level engine surface (`src/libbox64.c`, `src/signal_policy.c`,
`src/signal_route.c`, `src/thunk.c`, `thunk_la64.S`) keeps:

- the register file, `Push64`/`Pop64`, `DynaCall`/`DynaCall_abi`;
- the signal handlers and the per-frame signal-mask policy;
- the host-fault routing;
- the `setjmp`/`longjmp` regions;
- the lifecycle orchestration and the admission gate.

## 6. Foreign jumps and panics

Structural rule: **`setjmp` and its matching `longjmp` live in the same C
function, and no live Rust frame is between them.** A `longjmp` that skipped a
Rust frame would skip `Drop` (leaving the recursive context lock permanently
held) and leave `RefCell` borrow flags set; that is undefined-behaviour class,
not a style issue.

Within libbox64dl the engine's own signal-driven `siglongjmp` targets a frame
inside box64's `EmuRun`, deeper than the call orchestration and the admission
gate. The Rust phase leaves return before any C engine call starts, so they
cannot be crossed. `panic = "abort"` (see `rust/Cargo.toml`) removes the unwind
hazard, but it does **not** make a foreign `longjmp` safe: that is handled solely
by the structural rule above.

Known gaps (not claimed fixed):

- No exhaustive static inventory of every `setjmp`/`longjmp`/`siglongjmp` in the
  vendored box64 tree has been done.
- No ThreadSanitizer run has been done for this engine.
- The admission gate is fail-safe but not a synchronisation guarantee for the
  non-guest "cold config" entries (per-library signal policy, ranges): those keep
  the documented sequential-config contract plus the phase check.
- C++ exceptions from a host embedder crossing Rust FFI frames are not covered.
