/* Public embed API — host (native) side loads/calls x86_64 ELF via box64. */
#ifndef LIBBOX64_H
#define LIBBOX64_H

#include <stddef.h>
#include <stdint.h>

/* Engine ABI types and constants: box64_fpr_t, box64_call_results_t,
 * box64_call_desc_t and the engine
 * capacity constants. Defined once for the engine, unilib's generated-stub
 * seam (unilib.h aliases the unilib_* spellings to these types) and
 * uninode's ELF generator. */
#include "libbox64_abi.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef BOX64_API
#define BOX64_API __attribute__((visibility("default")))
#endif

BOX64_API int box64_lib_init(void);
BOX64_API void box64_lib_fini(void);
/* Rust owns the engine phase (UNINIT/INITIALIZING/READY/FAILED/FINAL); this is
 * non-zero only in READY. FAILED and FINAL are terminal: a later
 * box64_lib_init() is explicitly refused with a clear reason in
 * box64_dlerror() (final shutdown does not support re-initialisation). C API
 * guards (dlsym/dlclose/call) use this or the borrow-validity of the live
 * context, never a second state copy. */
BOX64_API int box64_is_ready(void);
/* Ensure the current thread has a properly sized guest emu (8 MiB stack),
 * instead of box64's 256 KiB fallback used by RunFunctionFmt. */
BOX64_API int box64_ensure_emu(void);

/* Diagnostic read of the Rust-owned phase as a stable integer: 0 UNINIT,
 * 1 INITIALIZING, 2 READY, 3 FAILED, 4 CLOSING, 5 FINAL. This reads the same
 * single phase word as box64_is_ready(); it is not a second state copy. */
BOX64_API uint8_t box64_engine_phase(void);

BOX64_API void* box64_dlopen(const char* path, int flags);
BOX64_API void* box64_dlsym(void* handle, const char* name);
BOX64_API int box64_dlclose(void* handle);
BOX64_API const char* box64_dlerror(void);

/* Unified forward-call entries.  BOX64_CALL_OK means the guest executed;
 * every other status means it did not and output objects must be ignored.
 * A guest longjmp follows the engine's non-local-return rule and does not
 * return through these entry points. */
BOX64_API box64_call_status_t box64_call_simple(
    void* sym, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
    uint64_t a4, uint64_t a5, uint64_t* rax_out);
BOX64_API box64_call_status_t box64_call_desc(
    void* sym, const box64_call_desc_t* desc, box64_call_results_t* out);

/* The descriptor is the only complex forward-call entry.  Its masks name
 * fixed physical engine positions and its status is independent of guest RAX.
 * See libbox64_abi.h for the wire layout and validation fields. */
/*
 * Diagnostics for the library's own per-call tracing. The "[box64] ..." lines
 * are opt-in and default OFF: the level is read once from BOX64DL_DIAG
 * ("0"/unset = off) on first use, and box64_diag_set() overrides it (0
 * silences, >0 enables). Errors never go through this switch; they are
 * reported through box64_dlerror().
 */
BOX64_API void box64_diag_set(int level);
BOX64_API int box64_diag_get(void);

/*
 * Signal-handling policy for the embedded engine (see patches/0005 and
 * src/signal_policy.c for the full rationale).
 *
 * The engine routes SIGSEGV/SIGBUS/SIGILL/SIGABRT and each top-level guest call
 * used to snapshot the signal mask with `sigsetjmp(buf, 1)`, i.e. one
 * rt_sigprocmask syscall per call just to arm the signal-driven resume path.
 * The policy selects how that is done:
 *
 *   FAST (default)    `sigsetjmp(buf, 0)` (no syscall). On every signal-driven
 *                     resume the handler restores the mask the kernel recorded
 *                     in the delivered signal's ucontext, i.e. the mask in
 *                     effect when the signal was delivered (the pre-delivery
 *                     mask). It can differ from the EmuRun-entry mask because
 *                     the guest, the host or the engine may change the mask
 *                     mid-call.
 *   COMPAT            `sigsetjmp(buf, 1)`; a later siglongjmp restores the mask
 *                     saved at setjmp time (the EmuRun-entry mask). The old
 *                     behaviour, still selectable by an embedder that needs it.
 *
 * FAST is sound for box64's own handlers (SIGSEGV/SIGBUS/SIGILL/SIGABRT, all
 * installed with SA_NODEFER and an empty sa_mask) and for a guest-installed
 * handler that adds its own sa_mask bits: restoring the kernel ucontext mask
 * removes exactly those, and a signal is only delivered while unblocked. See
 * src/signal_policy.c for the full argument.
 *
 * The engine deliberately exposes no "do not install the handlers" policy: the
 * dynarec write-protects guest code (protectDB) and repairs the resulting
 * SIGSEGV through those handlers, and guest fatal-signal/SEH emulation is
 * dispatched by them too. There is no handler-less policy: an unknown value
 * (including the old "none") is refused at init with box64_dlerror() carrying
 * the reason; there is no API value promising it.
 *
 * The global default is one-shot and must be selected before box64_lib_init():
 * either call box64_signal_policy_set() first, or set BOX64DL_SIGNAL_POLICY to
 * "compat"|"fast" (also read once, at init). The value is published into a
 * plain int before any guest call can run; the hot path reads it without a lock
 * or getenv. The default is FAST; COMPAT keeps the old exact behaviour.
 *
 * Priority for a given call: explicit per-library policy > environment library
 * rule > global default.
 */
typedef enum box64_signal_policy {
  BOX64_SIGNAL_POLICY_COMPAT = 0,
  BOX64_SIGNAL_POLICY_FAST = 1
} box64_signal_policy_t;

/* 0 on success; -1 (with box64_dlerror() set) on an out-of-range value or when
 * called after the global default has been published. */
BOX64_API int box64_signal_policy_set(int policy);
/* The published global default; FAST before init when none was selected. */
BOX64_API int box64_signal_policy_get(void);

/*
 * PER-LIBRARY POLICY. A guest call selects the policy of the library that owns
 * its entry symbol. A "scope" is the target of the current HOST entry — an
 * internal guest->guest call does not re-select per instruction — so this is
 * deliberately NOT absolute isolation: a library running under one policy can
 * call into another library without switching.
 *
 * box64_signal_policy_set_library(handle, policy) sets an explicit override for
 * an open box64_dlopen() handle. It returns 0, or -1 with box64_dlerror() set
 * for an invalid/closed handle, a wrapped library with no ELF image, or an
 * out-of-range policy. Obtaining a handle means the library is already loaded
 * and its constructors have ALREADY run under the environment rule or the
 * global default, so this applies to later host entries only; an environment
 * rule is what covers constructors. box64_signal_policy_get_library(handle)
 * returns the effective policy (override, matching rule, or global default), or
 * -1 with box64_dlerror() set.
 *
 * CONFIG CONTRACT: changing a library policy, and any load/unload that updates
 * the policy table, is a cold operation. The engine takes no hot-path lock; the
 * change is published atomically and every thread's O(1) policy cache is
 * invalidated by an epoch bump. No guest entry may run concurrently with it on
 * ANY thread, including a same-thread signal handler that would re-enter the
 * engine. The caller serializes configuration with guest execution (the same
 * sequential-config contract box64_lib_fini() documents); a call already in
 * flight keeps the policy of the frame it entered.
 *
 * ENVIRONMENT LIBRARY RULES: BOX64DL_SIGNAL_LIBRARY_RULES is a ';'-separated
 * list of MATCH=policy pairs, for example
 *
 *   BOX64DL_SIGNAL_LIBRARY_RULES='libfoo.so=compat;/opt/bar/libbaz.so=fast'
 *
 * A MATCH containing '/' is compared to the full library path (or its realpath)
 * exactly; any other MATCH is compared to the exact final path component. There
 * is no substring, prefix or glob matching, so 'foo.so' does not match
 * 'libfoo.so' and 'bar.so' does not match 'libbar.so.1'. The first matching
 * rule wins. A malformed rule (an unknown policy, a missing '=') makes
 * box64_lib_init() FAIL with box64_dlerror() set; it is never silently skipped,
 * so a typo cannot leave a library on the FAST default by accident. Rules are
 * bound in the engine before a library's constructors run, so a rule covers
 * constructors as well as later calls.
 *
 * CAPACITY: at most 64 distinct libraries may carry a non-default policy at
 * once (environment rules and explicit overrides share the bound). A basename
 * rule can match many images, so a large rule set can reach it. Exceeding it
 * never silently falls back to FAST: a load that cannot register its policy
 * refuses the process rather than run under the wrong one, and an explicit
 * box64_signal_policy_set_library() returns -1 with box64_dlerror() set.
 *
 * SCOPE (ELF and PE): a BOX64DL_SIGNAL_LIBRARY_RULES rule matches both formats
 * and both feed the same range registry. On the ELF side the engine binds a
 * rule in RunElfInit before the image's constructors run. On the PE side the
 * Wine adapter registers a module after LdrLoadDll returns, so that module's
 * DllMain and image-TLS callbacks run under the global default and the rule
 * covers only its later exported calls. The engine does not discover PE loads
 * itself: a foreign loader must register (base, size) through the
 * box64_signal_policy_set_range()/clear_range() entries in libbox64_ext.h; the
 * bundled winedl adapter does this for modules loaded through winedl_dlopen().
 */
BOX64_API int box64_signal_policy_set_library(void* handle, int policy);
BOX64_API int box64_signal_policy_get_library(void* handle);

/*
 * Per-call signal diagnostics, for tests and profiling only.
 *
 * setjmp_saves/setjmp_skips count actual SigSetJmp() arming per thread (a FAST
 * frame arms with savemask=0, a COMPAT frame with 1); they are maintained only
 * when box64_signal_stats_enable(1) was called (or BOX64DL_SIGNAL_STATS was
 * set). mask_restores counts FAST slow-path restores process-wide and is always
 * maintained (a relaxed atomic increment, only on the signal path — never on
 * the call path). publishes is the number of successful global-default
 * publications (process-wide; 0 or 1). `out` must not be NULL.
 */
typedef struct box64_signal_stats {
  unsigned long setjmp_saves;
  unsigned long setjmp_skips;
  unsigned long mask_restores;
  int publishes;
} box64_signal_stats_t;

BOX64_API void box64_signal_stats_enable(int on);
BOX64_API void box64_signal_stats(box64_signal_stats_t* out);

/* Host function -> guest-callable address, six integer registers forwarded. */
BOX64_API void* box64_callback(void* host_fn);

/*
 * Same, but the caller supplies the wrapper that runs on the guest's behalf:
 *
 *   void my_bridge(void* emu, uintptr_t host_fn);
 *
 * It gets the emulator handle and the host function, and moves arguments and
 * the return value itself with the two accessors below. This is how a callback
 * whose parameters are not all integers gets described — the default wrapper
 * only knows how to forward six integer registers.
 */
BOX64_API void* box64_callback_with(void* host_fn, void* bridge_fn);

/*
 * Declare a mapping the EMBEDDER made directly (host mmap/mprotect) as guest
 * memory with the given protection.
 *
 * box64 keeps its own protection map and its interpreter's CheckExec() refuses
 * to run guest code on a page that map does not describe as PROT_READ|PROT_EXEC
 * (it raises a synthetic SIGSEGV, code 0xecec). Mappings created by box64
 * itself, and PE/ELF images wine loads through the guest side, are recorded
 * automatically; a guest-addressed image the embedder maps with the raw host
 * mmap is not — e.g. winedl's in-memory main image, whose export stubs the
 * guest jumps into. This publishes that range so the stubs are executable.
 *
 * `prot` is in the host PROT_* encoding and is recorded VERBATIM, exactly like
 * box64's own setProtection(): this call does not map, mprotect or probe any
 * page. The CALLER must therefore ensure the host pages really carry `prot`
 * before declaring it — an mprotect may be refused (LSM, seccomp, W^X policy)
 * without the syscall failing, and a declared-but-absent PROT_EXEC turns a loud
 * syscall error into a silent guest fault at the first branch. Verify the
 * effective protection out of band (e.g. /proc/self/maps) and treat a mismatch
 * as fatal rather than declaring an image that cannot run.
 *
 * Returns 0, or -1 (with box64_dlerror() set) when the engine is not ready,
 * an argument is NULL/empty, or `prot` is 0 (which would publish nothing).
 */
BOX64_API int box64_declare_host_mapping(void* addr, size_t size, uint32_t prot);

/* Argument registers as the guest left them: 6 integer, 8 SSE. */
BOX64_API void box64_bridge_args(void* emu, uint64_t* gregs, uint64_t* fregs);
/* Return registers the guest will read: rax, rdx, xmm0, xmm1. */
BOX64_API void box64_bridge_ret(void* emu, uint64_t rax, uint64_t rdx,
                                const uint64_t* fregs2);

/*
 * The native function behind an address, for a provider that rewrites a guest
 * callback (the native-module seam's GOM direction) and must pass an
 * already-native pointer through untouched instead of wrapping it again.
 *
 * This is the public form of the engine's internal GetNativeFnc(), and it is
 * deliberately NOT a bridge-only predicate. It reports "native" for either of
 * two shapes:
 *   - `addr` is one of the engine's bridge thunks: the bridged host function
 *     is returned;
 *   - `addr` is a host address the host loader can name with dladdr(): `addr`
 *     itself is returned unchanged.
 * Any other address (e.g. plain guest code) yields NULL. It only inspects the
 * representation at `addr`; it never calls anything.
 */
BOX64_API void* box64_native_function(void* addr);

#ifdef __cplusplus
}
#endif

#endif /* LIBBOX64_H */
