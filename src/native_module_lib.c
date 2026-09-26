/*
 * native_module_lib.c — build a wrapped library from a registered native
 * module descriptor.
 *
 * The descriptor (include/libbox64_native_module.h) is the provider's complete
 * guest symbol surface: a host_library, and a bridge table whose entries are
 * either ordinary (host symbol resolved with dlsym and called through the
 * engine wrapper for the entry's signature) or GOM (a provider handler called
 * through that same wrapper, with the emulator handle as its first argument).
 *
 * This file owns no policy and no second ABI rule: the marshalling is box64's
 * generated wrapper, selected by signature token through
 * box64_native_module_wrapper(). The per-symbol lazy resolution mirrors what
 * box64's own wrapped path does (getSymbolInSymbolMaps), with the descriptor's
 * handler standing in for the "my_" lookup.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <dlfcn.h>
#include <link.h>
#include <stdint.h>
#include <string.h>

#include "box64context.h"
#include "bridge.h"
#include "custommem.h"
#include "debug.h"
#include "libbox64.h"
#include "libbox64_native_module.h"
#include "library.h"
#include "librarian/library_inner.h"
#include "librarian/library_private.h"
#include "native_module_lib.h"
#include "wrappedlibs.h"

/* Per-library cache: the descriptor is caller-owned and must stay const, so the
 * lazy resolution state lives here, one slot per descriptor bridge entry. A
 * slot is 0 until its bridge exists (a bridge address is never 0). */
typedef struct nm_lib_s {
  const box64_native_module* module;
  uintptr_t* resolved;
  void* host;
} nm_lib_t;

static int nm_lib_resolve(nm_lib_t* st, library_t* lib, const char* name,
                          int noweak, uintptr_t* offs, size_t* sz, int* weak) {
  const box64_native_module* m = st->module;
  size_t i;
  (void)noweak;
  for (i = 0; i < m->bridge_count; ++i) {
    const box64_bridge_entry* entry = &m->bridges[i];
    if (!entry->name || strcmp(entry->name, name) != 0) continue;
    if (!st->resolved[i]) {
      wrapper_t w = (wrapper_t)box64_native_module_wrapper(entry->signature);
      void* fnc = NULL;
      if (!w) {
        printf_log(LOG_INFO,
                   "native module %s: no engine wrapper for signature %s "
                   "(symbol %s)\n",
                   m->name, entry->signature ? entry->signature : "?",
                   entry->name);
        return 0;
      }
      if (entry->flags & BOX64_BRIDGE_GOM) {
        fnc = entry->handler;
        if (!fnc) {
          printf_log(LOG_INFO, "native module %s: GOM entry %s has no handler\n",
                     m->name, entry->name);
          return 0;
        }
      } else {
        const char* host_symbol =
            (entry->host_symbol && entry->host_symbol[0]) ? entry->host_symbol
                                                          : entry->name;
        fnc = dlsym(st->host, host_symbol);
        if (!fnc) {
          printf_log(LOG_INFO, "native module %s: host symbol %s not found\n",
                     m->name, host_symbol);
          return 0;
        }
      }
      st->resolved[i] = AddBridge(lib->w.bridge, w, fnc, 0, entry->name);
    }
    *offs = st->resolved[i];
    *sz = sizeof(void*);
    *weak = (entry->flags & BOX64_BRIDGE_WEAK) ? 1 : 0;
    return 1;
  }
  return 0;
}

static int nm_lib_getweak(library_t* lib, const char* name, uintptr_t* offs,
                          uintptr_t* sz, size_t asked_size, int* weak,
                          int* version, const char** vername, int local,
                          int* veropt, void** elfsym) {
  int wk = 0;
  (void)asked_size; (void)version; (void)vername; (void)local; (void)veropt;
  if (elfsym) *elfsym = NULL;
  if (!nm_lib_resolve((nm_lib_t*)lib->w.priv, lib, name, 0, offs, sz, &wk))
    return 0;
  /* Mirror WrappedLib_GetWeak(): a non-weak definition is not a weak one, so
   * the weak lookup must not report it (the global lookup owns it). */
  if (!wk) return 0;
  *weak = 1;
  return 1;
}

static int nm_lib_getglobal(library_t* lib, const char* name, uintptr_t* offs,
                            uintptr_t* sz, size_t asked_size, int* weak,
                            int* version, const char** vername, int local,
                            int* veropt, void** elfsym) {
  int wk = 0;
  (void)asked_size; (void)version; (void)vername; (void)local; (void)veropt;
  if (elfsym) *elfsym = NULL;
  if (!nm_lib_resolve((nm_lib_t*)lib->w.priv, lib, name, 1, offs, sz, &wk))
    return 0;
  if (wk) return 0; /* a weak descriptor entry is not a global definition */
  *weak = 0;
  return 1;
}

/* Mirrors WrappedLib_GetLocal(): a wrapped module exposes no local-binding
 * symbols, registered or not. */
static int nm_lib_getlocal(library_t* lib, const char* name, uintptr_t* offs,
                           uintptr_t* sz, size_t asked_size, int* weak,
                           int* version, const char** vername, int local,
                           int* veropt, void** elfsym) {
  (void)lib; (void)name; (void)offs; (void)sz; (void)asked_size; (void)weak;
  (void)version; (void)vername; (void)local; (void)veropt; (void)elfsym;
  return 0;
}

static void nm_lib_fini(library_t* lib) {
  nm_lib_t* st = (nm_lib_t*)lib->w.priv;
  lib->w.priv = NULL;
  if (st) {
    if (st->resolved) box_free(st->resolved);
    box_free(st);
  }
  WrappedLib_FinishFini(lib);
}

/*
 * Three-way result (see native_module_lib.h):
 *    0  the soname is not ours: the caller's static wrapped table decides;
 *    1  claimed and the runtime wrapped library is built;
 *   -1  claimed but the load failed closed. The caller must abort: a same-named
 *       guest ELF in the search path must not be loaded, no guest constructors
 *       may run, and no half-initialised LIB_WRAPPED may be left behind. Every
 *       allocation and library mutation this function made is undone before it
 *       returns -1.
 */
int box64dl_native_module_init_wrapped(struct library_s* lib,
                                       struct box64context_s* context) {
  const char* module_name;
  const box64_native_module* module;
  nm_lib_t* st;
  linkmap_t* lm;
  struct link_map* real_lm = NULL;

  (void)context;
  module_name = box64_native_module_for_soname(lib->name);
  if (!module_name) return 0; /* not ours: static wrapped table decides */
  module = box64_native_module_for_name(module_name);
  if (!module) return -1; /* claimed by soname but the descriptor vanished */

  st = (nm_lib_t*)box_calloc(1, sizeof(nm_lib_t));
  if (!st) {
    printf_log(LOG_NONE, "Error: out of memory for native module %s\n",
               module->name);
    return -1;
  }
  st->module = module;
  if (module->bridge_count) {
    st->resolved = (uintptr_t*)box_calloc(module->bridge_count, sizeof(uintptr_t));
    if (!st->resolved) {
      box_free(st);
      printf_log(LOG_NONE, "Error: out of memory for native module %s\n",
                 module->name);
      return -1;
    }
  }

  /* host_library == NULL means the running process (dlopen(NULL)): the
   * provider's ordinary symbols come from the embedder that loaded it. */
  st->host = dlopen(module->host_library, RTLD_LAZY | RTLD_GLOBAL);
  if (!st->host) {
    const char* err = dlerror();
    printf_log(LOG_NONE, "Error: native module %s: cannot open host library %s: %s\n",
               module->name,
               module->host_library ? module->host_library : "(process)",
               err ? err : "unknown");
    if (st->resolved) box_free(st->resolved);
    box_free(st);
    return -1; /* claimed and failed closed; never fall through to emulated */
  }

  /* Reserve the linkmap before publishing any wrapped-library state: a linkmap
   * failure then costs one dlclose instead of unwinding a half-built
   * LIB_WRAPPED (bridge table, symbol maps, fini hook) the caller must not
   * use. */
  lib->w.lib = st->host;
  lm = addLinkMapLib(lib);
  if (!lm) {
    printf_dlsym_dump(LOG_DEBUG, "Failure to add lib %s linkmap\n", lib->name);
    lib->w.lib = NULL;
    dlclose(st->host);
    if (st->resolved) box_free(st->resolved);
    box_free(st);
    return -1; /* claimed and failed closed */
  }

  WrappedLib_CommonInit(lib);
  lib->w.priv = st;
  lib->fini = nm_lib_fini;
  lib->getglobal = nm_lib_getglobal;
  lib->getweak = nm_lib_getweak;
  lib->getlocal = nm_lib_getlocal;
  lib->type = LIB_WRAPPED;
  lib->w.refcnt = 1;
  printf_dump(LOG_INFO, "Using native module %s for %s\n", module->name,
              lib->name);

#ifndef ANDROID
  if (dlinfo(lib->w.lib, RTLD_DI_LINKMAP, &real_lm)) {
    printf_dlsym_dump(LOG_DEBUG, "Failed to dlinfo lib %s\n", lib->name);
  }
#endif
  if (real_lm) {
    lm->l_addr = real_lm->l_addr;
    lm->l_name = real_lm->l_name;
    lm->l_ld = real_lm->l_ld;
  } else {
    lm->l_name = lib->path;
  }
  return 1;
}

/*
 * Public engine accessor for the native function behind an address.
 *
 * This is the public form of box64's internal GetNativeFnc(), which the ELF
 * provider's callback rewriting uses to pass an already-native callback
 * through untouched instead of wrapping it twice. "Already native" covers two
 * cases, and this call is deliberately NOT a bridge-only predicate:
 *
 *   - `addr` is one of the engine's bridge thunks: the bridged host function
 *     is returned;
 *   - `addr` is a host address the host loader can name with dladdr(): the
 *     address itself is returned unchanged.
 *
 * An address that is neither a bridge nor host-resolvable yields NULL. The
 * call only inspects the representation at `addr`; it never calls anything.
 */
void* box64_native_function(void* addr) {
  return GetNativeFnc((uintptr_t)addr);
}
