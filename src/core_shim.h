/*
 * core_shim.h — the private C shim between the Rust lifecycle owner and the
 * vendored box64 engine.
 *
 * Everything here is MECHANICAL: it moves values in and out of box64's private
 * structures and calls box64 functions that Rust cannot call directly (they
 * take/return box64context_t*, x64emu_t*, elfheader_t*, ...). It deliberately
 * holds NO policy state:
 *
 *   - phase (UNINIT/INITIALIZING/READY/FAILED/FINAL) lives in Rust
 *     (rust/src/phase.rs) and is the only writable copy;
 *   - the error slot lives in Rust (rust/src/error.rs);
 *   - `my_context` is box64's own global; the shim reads it as a borrow-validity
 *     fact (a plain pointer load, not synchronisation) and never as a second
 *     state source.
 *
 * Ownership rule that keeps this file free of double frees:
 *   - box64dl_core_end() delegates teardown to box64's own endBox64(), which
 *     frees the context exactly once (FreeBox64Context sets my_context = NULL);
 *   - box64dl_core_free_context() is the ONLY other free path and is used only
 *     when initialization failed after the context existed and box64's full
 *     teardown must not run;
 *   - Rust never frees the context itself, so no Rust Drop can race box64.
 *
 * Visibility: hidden. These symbols are internal glue between the Rust crate
 * and the C shim inside the same final artifact; they are not public ABI.
 */
#ifndef BOX64DL_CORE_SHIM_H
#define BOX64DL_CORE_SHIM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef BOX64DL_CORE_API
#if defined(__GNUC__)
#define BOX64DL_CORE_API __attribute__((visibility("hidden")))
#else
#define BOX64DL_CORE_API
#endif
#endif

/* One-time process environment bootstrap: page size, box64 env files, box64
 * trace setup, system info and the dynarec host check. Idempotent enough to be
 * called once per successful init; the Rust phase machine guarantees it is not
 * called twice without a successful teardown in between (which is refused). */
BOX64DL_CORE_API void box64dl_core_boot_env(void);

/* Publish the one-shot signal policy before any guest call. 0 on success;
 * non-zero on refusal, with the reason available from
 * box64dl_core_signal_policy_refusal(). */
BOX64DL_CORE_API int box64dl_core_signal_policy_publish(void);
BOX64DL_CORE_API const char* box64dl_core_signal_policy_refusal(void);

/* Create the box64 context (my_context). 0 on success, -1 on failure. */
BOX64DL_CORE_API int box64dl_core_context_new(void);
/* Rebind context->box64lib to the module that owns `box64_fn` (the address of
 * the C box64_lib_init). 0 on success, -1 on failure. Failure is not fatal
 * for the engine but is reported so the caller can decide. */
BOX64DL_CORE_API int box64dl_core_rebind(const void* box64_fn);

/* Fill the embedded-context guest environment (argv/auxv/box64path/ld paths).
 * No failure mode: it either fills the fields or leaves them usable. */
BOX64DL_CORE_API void box64dl_core_init_guest(void);
/* Register the current host mappings with box64's protection map. */
BOX64DL_CORE_API void box64dl_core_load_protection(void);
/* Give the context its per-thread guest stack size (8 MiB embedded default). */
BOX64DL_CORE_API void box64dl_core_init_stack(void);

/* Create this thread's emu with a full-size stack and register it in box64's
 * per-thread TLS. 0 on success, -1 on failure (error slot carries the reason). */
BOX64DL_CORE_API int box64dl_core_new_emu(void);

/* Non-zero while the C-side context exists (plain load; sequential guard only). */
BOX64DL_CORE_API int box64dl_core_context_live(void);
/* The C-side context pointer as an opaque value (NULL when absent). Rust uses
 * it only as an identity handle; it never dereferences it. */
BOX64DL_CORE_API void* box64dl_core_context_ptr(void);

/* Full teardown via box64's own endBox64(). Returns 1 when the context was
 * freed (my_context == NULL afterwards) and 0 when endBox64 refused to free it
 * (e.g. emu workers still alive); on 0 the context is NOT freed and the process
 * must exit rather than reuse the engine. */
BOX64DL_CORE_API int box64dl_core_end(void);

/* Init-failure cleanup: FreeBox64Context(my_context). Only valid when the
 * context exists and box64's full teardown has not run. Never call after a
 * successful box64dl_core_end(). */
BOX64DL_CORE_API void box64dl_core_free_context(void);

/* Per-thread probes that never fabricate an emu (box64's thread_get_emu would
 * create a 256 KiB one). box64dl_core_peek_et() returns this thread's
 * emuthread_t* or NULL; box64dl_core_get_emu() returns this thread's emu or
 * NULL. */
BOX64DL_CORE_API void* box64dl_core_peek_et(void);
BOX64DL_CORE_API void* box64dl_core_get_emu(void);

#ifdef __cplusplus
}
#endif

#endif /* BOX64DL_CORE_SHIM_H */
