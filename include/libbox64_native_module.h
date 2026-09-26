/*
 * libbox64_native_module — the ENGINE side of the explicit native-module seam.
 *
 * The provider side is a fixed, compiled-in module descriptor (uninode's
 * native/uninode_native_module.h): a module name, the guest sonames it
 * satisfies, the host library its ordinary symbols come from, and a bridge
 * table (guest symbol -> signature -> host symbol / provider handler). This
 * header is the engine's mirror of that descriptor and the entries the
 * provider links against:
 *
 *     box64_register_native_module(const box64_native_module*, unsigned)
 *     box64_native_module_for_soname(const char*)
 *     box64_native_module_for_name(const char*)
 *     box64_native_module_count(void)
 *     box64_native_module_wrapper(const char*)     (engine -> provider query)
 *
 * WHY THIS EXISTS
 * ---------------
 * A provider must make host symbols visible to an x86_64 guest naming a
 * soname the engine's wrapped-library table does not list (libnode.so for a
 * guest N-API addon). The historical route was a build-time graft: the
 * provider's wrapped*.c was symlinked into box64's src/wrapped and its
 * library_list.inc into library_list_extra.h, so the engine's BUILD read the
 * provider's source tree. This seam removes that reverse build dependency: the
 * provider registers a descriptor at run time (before the first
 * box64_dlopen()), and the engine builds the wrapped library from it.
 *
 * WHAT THE ENGINE CONSUMES
 * ------------------------
 * When the wrapped-library load path sees a guest soname that no static
 * wrapped module claims, it consults box64_native_module_for_soname(). If a
 * descriptor claims the soname, the engine builds a wrapped library whose
 * symbol table is exactly that descriptor's bridge table:
 *
 *   - an ORDINARY entry (no BOX64_BRIDGE_GOM) resolves its host symbol with
 *     dlsym() against the descriptor's host_library and bridges it to the
 *     guest through the engine wrapper for the entry's signature;
 *   - a GOM entry carries a provider handler called through the engine wrapper
 *     for the entry's E-form signature (the handler receives the emulator
 *     handle as its first argument, exactly like a compiled-in my_* wrapper).
 *     The handler owns the host call and the callback rewriting; the engine
 *     only marshals the guest arguments.
 *   - an entry whose signature the engine cannot wrap, or whose host symbol
 *     cannot be resolved, makes the symbol resolution fail closed (the load
 *     reports a missing symbol instead of handing back garbage).
 *
 * SHAPE (deliberately fixed, not a registry)
 * ------------------------------------------
 * - No directory scan, no config file, no dynamic loading, no generic
 *   registry: a small fixed-size table holds up to BOX64_NATIVE_MODULE_MAX
 *   module identities.
 * - The descriptor layout is pinned here and by uninode's provider header;
 *   box64_register_native_module() refuses an unknown ABI version instead of
 *   misreading the struct.
 * - box64_native_module_wrapper() is the engine's signature->wrapper lookup.
 *   It is generic: the engine owns a documented set of SysV signature tokens
 *   (src/native_module_wrappers_private.h) and refuses any other token.
 *
 * CONFIG CONTRACT
 * ---------------
 * Registration is a cold, pre-guest-load operation (the same sequential-config
 * contract as box64_signal_policy_set()): call it once per process before the
 * first box64_dlopen(). It is not thread-safe and must not run concurrently
 * with any guest execution. The engine copies no strings; the descriptor, its
 * soname pointers and its bridge table must have static storage duration.
 */
#ifndef LIBBOX64_NATIVE_MODULE_H
#define LIBBOX64_NATIVE_MODULE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef BOX64_API
#define BOX64_API __attribute__((visibility("default")))
#endif

/* ABI version of the descriptor layout below. Passed to the engine entry. */
#define BOX64_NATIVE_MODULE_ABI 2u

/* Fixed capacity. One entry per distinct module name; no growth, no registry. */
#define BOX64_NATIVE_MODULE_MAX 4u

/* `handler` is a provider GOM handler, not a host symbol name: the engine
 * bridges it through the engine wrapper for the entry's signature, so the
 * handler is called as handler(emu, <guest args...>). Without this flag the
 * entry is ordinary: the engine resolves host_symbol (or name when that is
 * NULL) with dlsym() against the descriptor's host_library. */
#define BOX64_BRIDGE_GOM 1u
/* The symbol is weak: the wrapped path reports "not found" instead of a hard
 * miss when the signature/host symbol cannot be resolved. */
#define BOX64_BRIDGE_WEAK 2u

/* One guest symbol of a registered module. `signature` is a box64 SysV
 * signature token ("iFppLp", "iFEppLppp", ...): the exact name of the engine
 * wrapper function box64_native_module_wrapper() returns. A GOM entry uses the
 * E-form (emulator handle first); the descriptor's ordinary entries do not. */
typedef struct box64_bridge_entry {
  /* Guest-visible symbol name, never NULL/empty. Static storage. */
  const char* name;
  /* Engine wrapper signature token, never NULL/empty. Static storage. */
  const char* signature;
  /* Host symbol dlsym() target for an ordinary entry; NULL means `name`.
   * Ignored for a GOM entry (its handler owns the host call). Static storage. */
  const char* host_symbol;
  /* Provider GOM handler. NULL for an ordinary entry, required for a GOM one.
   * The handler's C signature must match the entry's signature with the
   * emulator handle as its first argument. */
  void* handler;
  /* BOX64_BRIDGE_GOM / BOX64_BRIDGE_WEAK (0 for a normal ordinary entry). */
  unsigned flags;
} box64_bridge_entry;

/* Mirror of uninode's uninode_native_module. Field order and width are the
 * wire contract; both sides pin them with static assertions in their tests. */
typedef struct box64_native_module {
  /* Diagnostic name, never NULL/empty. Also the module identity the wrapped
   * path resolves against (see libbox64_native_module_for_soname). */
  const char* name;
  /* Guest sonames this module satisfies, e.g. "libnode.so". Static storage. */
  const char* const* sonames;
  size_t soname_count;
  /* Host library the ORDINARY entries are dlsym()'d from: a dlopen() target
   * path/soname, or NULL for the running process (dlopen(NULL)). Static. */
  const char* host_library;
  /* Bridge table: the module's complete guest symbol surface. */
  const box64_bridge_entry* bridges;
  size_t bridge_count;
} box64_native_module;

/*
 * Register one module identity with the engine.
 *
 * Returns 0 on success, non-zero when the engine refuses: NULL/malformed
 * descriptor, a count without its table, a malformed bridge entry (missing
 * name/signature, or a GOM entry without a handler), two bridge entries with
 * the same guest symbol name, a duplicate module name, an unknown abi_version,
 * or the fixed table is full. Refusal is fail-closed: the caller must not
 * treat a refused module as registered. The engine owns duplicate handling;
 * this provider keeps no registry.
 */
BOX64_API int box64_register_native_module(const box64_native_module* module,
                                           unsigned abi_version);

/*
 * The module name that claims `soname`, or NULL when none does. This is the
 * lookup the wrapped-library load path consults. Exact match, no glob/prefix.
 */
BOX64_API const char* box64_native_module_for_soname(const char* soname);

/* The registered descriptor with this module name, or NULL. The engine uses it
 * to build the runtime wrapped library; it is public so a provider can observe
 * what the engine recorded. Exact match. */
BOX64_API const box64_native_module* box64_native_module_for_name(
    const char* name);

/* Number of registered modules, for diagnostics and tests. */
BOX64_API unsigned box64_native_module_count(void);

/*
 * The engine wrapper function for a signature token, as a wrapper_t
 * (void (*)(void* emu, uintptr_t fnc)), or NULL when the engine does not carry
 * that wrapper. The wrapped path uses this to build a bridge for a descriptor
 * entry; a provider can use it to observe the engine's signature support.
 * The returned pointer is a real wrapper function, not a descriptor.
 */
BOX64_API void* box64_native_module_wrapper(const char* signature);

#ifdef __cplusplus
}
#endif

#endif /* LIBBOX64_NATIVE_MODULE_H */
