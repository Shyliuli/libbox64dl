/*
 * test_bridge_wiring.c — the REAL-ENGINE half of the native-module seam.
 *
 * tests/test_native_module_registration.c proves the registration protocol with
 * a plain host compiler. This test links the full engine archive (the same
 * shape a host embedder uses) and proves the properties only the real
 * wrapped-library load path and the engine's generated wrapper set can show:
 *
 *   1. FAIL-CLOSED WITHOUT A SEAM: a guest soname that no wrapped module and no
 *      registered descriptor claims does not load, and the engine reports an
 *      error instead of handing back a handle.
 *   2. THE BRIDGE TABLE IS CONSUMED: an ordinary entry's host symbol is
 *      dlsym()'d from the descriptor's host_library and bridged; a GOM entry's
 *      provider handler is bridged through the engine wrapper for its E-form
 *      signature; an entry naming a signature the engine does not carry, or a
 *      host symbol that does not exist, fails closed.
 *   3. THE ENGINE CARRIES THE PROVIDER'S REAL SIGNATURES: the signature ->
 *      wrapper lookup answers the ordinary AND the GOM E-form tokens the
 *      provider bridge table uses (iFppp, iFEppp, iFEppLppp,
 *      iFEppppLLppppp, ...), and refuses an unknown token.
 *   4. A CLAIMED LOAD FAILS CLOSED: a descriptor that claims a soname but
 *      cannot build (its host library does not dlopen) makes box64_dlopen()
 *      return NULL instead of silently loading a same-named x86_64 guest ELF
 *      from the search path and running its constructors. The test chdir()s
 *      into a fixture directory holding exactly such a guest file, and loads a
 *      control file first to prove the fixture is genuinely loadable.
 *
 * The soname is deliberately NOT in any plugin's library_list.inc, so success
 * can only come from the registered descriptor, not from a static table entry.
 *
 * Build/run: make check-bridge-registration
 */
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "libbox64.h"
#include "libbox64_native_module.h"

#ifndef RTLD_NOW
#define RTLD_NOW 0x00002
#endif
#ifndef RTLD_GLOBAL
#define RTLD_GLOBAL 0x00100
#endif

/* A real host symbol the ordinary entry can resolve from the process. It is
 * exported by this test binary (-rdynamic on the link line), so the descriptor's
 * NULL host_library (dlopen(NULL)) finds it without any external library. */
int seam_host_target(void* a, void* b) {
  (void)a;
  (void)b;
  return 42;
}

/* A provider GOM handler; the engine bridges it, it is never called here. */
int seam_gom_handler(void* emu, void* a, void* b, void* c) {
  (void)emu;
  (void)a;
  (void)b;
  (void)c;
  return 0;
}

static const char* const seam_sonames[] = {"libseam_wrapped.so"};
static const box64_bridge_entry seam_bridges[] = {
    /* Ordinary: host symbol resolved with dlsym from host_library. */
    {"seam_ordinary", "iFpp", "seam_host_target", NULL, 0},
    /* GOM: provider handler bridged through the E-form wrapper. */
    {"seam_gom", "iFEppp", "seam_host_target", (void*)&seam_gom_handler,
     BOX64_BRIDGE_GOM},
    /* Signature the engine does not carry: must fail closed, not crash. */
    {"seam_badsig", "wFppp", NULL, NULL, 0},
    /* Host symbol that does not exist: must fail closed. */
    {"seam_missing", "iFpp", "seam_no_such_host_symbol", NULL, 0},
};
static const box64_native_module seam_module = {
    "seamhost",
    seam_sonames,
    1,
    NULL, /* the running process */
    seam_bridges,
    sizeof(seam_bridges) / sizeof(seam_bridges[0]),
};

/* A descriptor that CLAIMS a soname but cannot build its wrapped library: the
 * host library does not exist, so the engine's host dlopen fails. The fixture
 * directory contains a real x86_64 ELF under exactly this soname; a claimed
 * failure must not silently fall through to that emulated file. */
static const char* const seam_broken_sonames[] = {"libseam_claimed.so"};
static const box64_native_module seam_broken_module = {
    "seambroken", seam_broken_sonames, 1,
    "libseam_no_such_host_library.so", /* host dlopen must fail */
    NULL, 0};

static int g_fails;
static void check(int cond, const char* what) {
  if (cond) {
    printf("[bridge-wiring] ok   %s\n", what);
  } else {
    printf("[bridge-wiring] FAIL %s\n", what);
    g_fails++;
  }
}

int main(int argc, char** argv) {
  void* absent;
  void* handle;
  void* sym;

  if (box64_lib_init() != 0) {
    printf("[bridge-wiring] FAIL engine init: %s\n",
           box64_dlerror() ? box64_dlerror() : "?");
    return 1;
  }

  /* 1. No seam: an unknown soname fails closed. */
  absent = box64_dlopen("libseam_absent.so", RTLD_NOW | RTLD_GLOBAL);
  check(absent == NULL && box64_dlerror() != NULL,
        "unclaimed soname fails closed with an engine error");

  /* 2. The engine entry is real and ABI-versioned. */
  check(box64_native_module_count() == 0, "engine starts with no module");
  check(box64_register_native_module(&seam_module,
                                     BOX64_NATIVE_MODULE_ABI + 1u) != 0,
        "real engine refuses an unknown descriptor ABI");
  check(box64_native_module_count() == 0, "ABI refusal recorded nothing");
  check(box64_register_native_module(&seam_module,
                                     BOX64_NATIVE_MODULE_ABI) == 0,
        "real engine accepts the descriptor");
  check(box64_native_module_count() == 1, "engine recorded one module");
  check(box64_native_module_for_soname("libseam_wrapped.so") != NULL &&
            strcmp(box64_native_module_for_soname("libseam_wrapped.so"),
                   "seamhost") == 0,
        "descriptor soname resolves to the module name");
  check(box64_native_module_for_name("seamhost") == &seam_module,
        "descriptor is retrievable by name with its bridge table");

  /* 3. The engine's signature -> wrapper lookup is real and covers the
   * provider's ordinary and GOM E-form tokens. */
  check(box64_native_module_wrapper("iFpp") != NULL,
        "engine wrapper for an ordinary signature");
  check(box64_native_module_wrapper("iFEppp") != NULL,
        "engine wrapper for a GOM E-form signature");
  check(box64_native_module_wrapper("iFEppLppp") != NULL,
        "engine wrapper for a wider GOM E-form signature");
  check(box64_native_module_wrapper("iFEppppLLppppp") != NULL,
        "engine wrapper for the widest GOM E-form signature");
  check(box64_native_module_wrapper("wFppp") == NULL,
        "unknown signature token is refused (fail-closed)");
  check(box64_native_module_wrapper(NULL) == NULL,
        "NULL signature token is refused");

  /* 4. The registered soname now loads through the wrapped path and answers
   * real host symbols with a guest-callable bridge. */
  handle = box64_dlopen("libseam_wrapped.so", RTLD_NOW | RTLD_GLOBAL);
  check(handle != NULL,
        "registered soname loads through the wrapped-library path");
  if (handle) {
    sym = box64_dlsym(handle, "seam_ordinary");
    check(sym != NULL, "ordinary entry resolves its dlsym'd host symbol");
    sym = box64_dlsym(handle, "seam_gom");
    check(sym != NULL, "GOM entry resolves to its provider handler bridge");
    sym = box64_dlsym(handle, "seam_badsig");
    check(sym == NULL, "entry with an unwrappable signature fails closed");
    sym = box64_dlsym(handle, "seam_missing");
    check(sym == NULL, "entry with a missing host symbol fails closed");
    sym = box64_dlsym(handle, "seam_not_in_table");
    check(sym == NULL, "symbol not in the bridge table fails closed");
    sym = box64_dlsym(handle, "seam_ordinary");
    check(sym != NULL,
          "a good symbol still resolves after the resolution failures");
    check(box64_dlclose(handle) == 0, "wrapped module closes");
  } else {
    printf("[bridge-wiring] dlopen error: %s\n",
           box64_dlerror() ? box64_dlerror() : "?");
  }

  /* 5. H1 fail-closed regression: a descriptor that CLAIMS a soname but cannot
   * build its wrapped library must not let a same-named x86_64 guest ELF in the
   * search path load (and run its constructors) through the emulated fallback.
   * The control load first proves the fixture really is loadable when no
   * descriptor claims it, so the refusal below is not vacuous. */
  if (argc < 2) {
    printf("[bridge-wiring] FAIL missing guest fixture directory argument\n");
    g_fails++;
  } else if (chdir(argv[1]) != 0) {
    printf("[bridge-wiring] FAIL cannot enter fixture dir %s\n", argv[1]);
    g_fails++;
  } else {
    unlink("seam_control_marker");
    unlink("seam_claimed_marker");

    handle = box64_dlopen("libseam_control.so", RTLD_NOW | RTLD_GLOBAL);
    check(handle != NULL,
          "control same-shape guest loads when no descriptor claims it");
    check(handle != NULL && access("seam_control_marker", F_OK) == 0,
          "control guest constructor actually ran (fixture is loadable)");
    if (handle) box64_dlclose(handle);

    check(box64_register_native_module(&seam_broken_module,
                                       BOX64_NATIVE_MODULE_ABI) == 0,
          "fail-closed descriptor registered");
    check(box64_native_module_count() == 2, "both modules recorded");

    handle = box64_dlopen("libseam_claimed.so", RTLD_NOW | RTLD_GLOBAL);
    check(handle == NULL,
          "claimed-but-unbuildable soname fails closed (no guest load)");
    check(handle == NULL && box64_dlerror() != NULL,
          "fail-closed load reports an engine error");
    check(access("seam_claimed_marker", F_OK) != 0,
          "same-named guest ELF was not loaded (constructor did not run)");
    if (handle) box64_dlclose(handle);

    unlink("seam_control_marker");
    unlink("seam_claimed_marker");
  }

  box64_lib_fini();

  if (g_fails) {
    printf("[bridge-wiring] %d FAILURES\n", g_fails);
    return 1;
  }
  printf("[bridge-wiring] all checks passed\n");
  return 0;
}
