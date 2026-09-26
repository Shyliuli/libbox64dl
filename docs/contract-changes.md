# E1 public C ABI changes (libbox64dl)

Scope: the first complete engine capability (E1). This file records the
deliberate breaks. It does not describe the old tree's code; the old tree is
read-only and is not modified by E1.

## Removed / changed

| Symbol | Before | After | Reason |
| --- | --- | --- | --- |
| `box64_lib_ready` | alias of `box64_is_ready` | **removed** | There is exactly one ready predicate. The old tree's in-repo consumer was `unilib/src/unilib.c` (`if (box64_lib_ready())`), which must call `box64_is_ready()`. This is a source/ABI break for any out-of-tree consumer. |
| `box64_callback` | `void* box64_callback(void* host_fn, const char* fmt)` | `void* box64_callback(void* host_fn)` | The `fmt` argument was never read (it was `(void)fmt;`). Removing it is a source/ABI break; callers must drop the second argument. |
| `box64_main` | declared in `include/libbox64.h` | declared in `cli/box64_main.h` | It is the packaged CLI / DSO entry that drives box64's own `initialize()`/`emulate()`; it is not part of the embeddable library ABI. |

## Added

| Symbol | Purpose |
| --- | --- |
| `box64_engine_phase` | Diagnostic read of the Rust-owned phase as a stable integer (0 UNINIT, 1 INITIALIZING, 2 READY, 3 FAILED, 4 CLOSING, 5 FINAL). |

## Semantics that changed without a signature change

- `box64_lib_init()`: idempotent while READY. After a failed init the phase is
  FAILED and after `box64_lib_fini()` it is FINAL; both are **terminal** and a
  later `box64_lib_init()` is refused with a reason in `box64_dlerror()`.
- `box64_lib_fini()`: **orchestrated in C** over the Rust phase leaves (it used
  to run in Rust). It publishes CLOSING before any teardown, and when a
  guest-capable call is still in flight it publishes the terminal FINAL state
  and returns **without freeing** the context; the error slot explains that the
  process must exit without reuse. `endBox64()` may also refuse (emu workers
  still alive); that is the same terminal FINAL state. On success the error slot
  is clear.
- `box64_is_ready()`: reads the Rust phase only. `my_context` is not part of the
  ready decision. The `call_entry` cached-emu fast path checks readiness before
  returning a cached emu, so teardown cannot be re-entered.

## Dynamic symbol table (not an ABI change, but a build contract)

The shipped DSO exports three dynamic classes: the public `box64_*` API, the
runtime-trampoline primitive `box64dl_thunk_*` (`include/box64dl_thunk.h`), and
the engine-internal `my*` wrapped-library table that box64's librarian resolves
with `dlsym(my_context->box64lib, "my_"+name)`. The `my*` class is thousands of
symbols (`my_`, `my2_`, `my32_` and the other altmy prefixes); it is not public
API, but it must be dynamic for any DT_NEEDED-libc guest to run. The
`box64dl_thunk_*` functions are dynamic because the engine owns the thunk
registry: a DSO consumer cannot build a guest->host callable thunk itself
(uninode's PE bridge links `box64dl_thunk_get`). The thunk asm template
(`box64dl_thunk_code`/`_end`), the Rust runtime and the private
`box64dl_core_*` shim stay local and are checked to be absent from the dynamic
table.

## Explicit native-module seam (ABI 2)

The seam that lets a sibling provider register a run-time wrapped library without
a build-time graft is now implemented; it is a deliberate ABI break of the
unreleased ABI-1 placeholder descriptor.

| Symbol | Before | After | Reason |
| --- | --- | --- | --- |
| `box64_native_module` | ABI 1: name + sonames + opaque `bridges`/`bridge_count` (identity only, never consumed) | ABI 2: adds `host_library` and a typed `box64_bridge_entry` table the wrapped path consumes | The engine must resolve real host symbols and call provider GOM handlers, not merely record an identity. |
| `box64_register_native_module` | accepted an opaque bridge table | validates every bridge entry (name/signature, GOM handler) | Fail-closed registration. |
| `box64_native_module_for_soname` | unchanged signature | unchanged | |
| `box64_native_module_for_name` | **added** | returns the registered descriptor | The runtime wrapped library is built from the descriptor. |
| `box64_native_module_wrapper` | **added** | signature token -> engine wrapper function | The engine owns the signature set (`src/wrappedbox64dnative_private.h`); the wrapper is box64's own generated wrapper, not a second ABI rule. |
| `box64_native_function` | **added** | public form of the internal `GetNativeFnc()` | A provider that rewrites guest callbacks must pass an already-native pointer through untouched. |

The `my*` dynamic class is unchanged: the seam does not add `my_` entries. The
new public symbols are `box64_*` and stay inside the existing version-script
class, so `tools/check_exports.py` still sees exactly the public ABI,
`box64dl_thunk_*` and `my*`.

## Unchanged (deliberately)

- `box64_call_simple` / `box64_call_desc` signatures, `box64_call_status_t`,
  `box64_call_desc_t`, `box64_call_results_t`, `BOX64_CALL_*` flags/masks and the
  inline `box64_call_desc_validate` rule.
- `box64_dlopen` / `box64_dlsym` / `box64_dlclose` / `box64_dlerror`.
- `box64_callback_with`, `box64_bridge_args`, `box64_bridge_ret`,
  `box64_bridge_sp`, `box64_declare_host_mapping`, `box64_set_box64path`,
  `box64_set_wine_mode`, the signal-policy surface and the detach/reset surface.
- `box64_sigplan_t` / `BOX64_SIGPLAN` (the plan POD used by the descriptor tests).
