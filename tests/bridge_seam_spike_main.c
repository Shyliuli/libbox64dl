/*
 * bridge_seam_spike_main.c — cross-repository spike for the native-module seam.
 *
 * This binary is the smallest thing that proves the seam end to end across the
 * repository boundary: it compiles uninode's REAL provider sources
 * (native/uninode_native_module.c + wrapped/wrappedlibnode.c, the latter
 * carrying the libnode bridge table and the GOM handlers) and links them
 * against the REAL libbox64dl engine archive. uninode's weak declaration
 * of box64_register_native_module() must resolve to the engine's strong
 * definition, the engine must accept the real libnode descriptor, and the
 * engine's lookup must return the module name that descriptor registered.
 *
 * One TU cannot include both descriptor headers: the engine's
 * box64_native_module and the provider's uninode_native_module are distinct C
 * types (layout-identical, but a C compiler refuses two different struct types
 * for one function). The engine's lookup is therefore reached through the
 * small adapter in tests/bridge_seam_spike_engine.c, which includes the engine
 * header; this TU includes only the provider header. The engine reading the
 * provider's descriptor at run time is the ABI proof that a pair of static
 * assertions in one TU could not give.
 *
 * Built and run by tools/bridge_seam_spike.sh (make bridge-seam-spike), which
 * takes the provider tree as a path and fails loudly when it is absent. It is
 * deliberately NOT part of the engine's default test target: it reaches across
 * repositories, and the engine's no-Node gate must stay provable on an
 * engine-only checkout.
 */
#include <stdio.h>
#include <string.h>

#include "libbox64.h"
#include "uninode_native_module.h"

/* Engine-side adapter (tests/bridge_seam_spike_engine.c): declared without any
 * descriptor type so this TU never sees the engine's struct. */
extern const char* bridge_seam_spike_lookup(const char* soname);
extern unsigned bridge_seam_spike_count(void);

static int g_fails;
static void check(int cond, const char* what) {
  if (cond) {
    printf("[bridge-seam-spike] ok   %s\n", what);
  } else {
    printf("[bridge-seam-spike] FAIL %s\n", what);
    g_fails++;
  }
}

int main(void) {
  char err[512];

  if (box64_lib_init() != 0) {
    printf("[bridge-seam-spike] FAIL engine init: %s\n",
           box64_dlerror() ? box64_dlerror() : "?");
    return 1;
  }

  check(uninode_native_module_engine_available() == 1,
        "uninode weak seam resolved to the real engine entry");

  err[0] = '\0';
  check(uninode_native_modules_register(err, sizeof(err)) == 0,
        "real engine accepts the real libnode descriptor");
  if (err[0]) printf("[bridge-seam-spike] provider error: %s\n", err);

  check(bridge_seam_spike_lookup("libnode.so") != NULL &&
            strcmp(bridge_seam_spike_lookup("libnode.so"), "libnode") == 0,
        "engine resolves a libnode soname to the registered module");
  check(bridge_seam_spike_lookup("libnode.so.115") != NULL,
        "engine resolves the versioned libnode soname");
  check(bridge_seam_spike_count() == 1,
        "engine recorded exactly the one provider module");

  box64_lib_fini();

  if (g_fails) {
    printf("[bridge-seam-spike] %d FAILURES\n", g_fails);
    return 1;
  }
  printf("[bridge-seam-spike] all checks passed\n");
  return 0;
}
