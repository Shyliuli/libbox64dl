/*
 * bridge_seam_spike_engine.c — engine-header TU for the cross-repository spike.
 *
 * tests/bridge_seam_spike_main.c holds the provider header; this TU holds the
 * engine header. Keeping them in separate translation units is required
 * because the two descriptor structs are distinct C types (see the main file).
 *
 * The engine's descriptor layout is pinned here with static assertions; the
 * provider's identity is then validated against it at run time by the main TU
 * registering the real libnode descriptor and asking the engine to resolve its
 * sonames.
 */
#include "libbox64_native_module.h"

#include <stddef.h>

_Static_assert(offsetof(box64_native_module, name) == 0, "name first");
_Static_assert(offsetof(box64_native_module, sonames) == sizeof(void*),
               "sonames after name");
_Static_assert(offsetof(box64_native_module, soname_count) == 2 * sizeof(void*),
               "soname_count after sonames");
_Static_assert(offsetof(box64_native_module, host_library) ==
                   3 * sizeof(void*),
               "host_library after soname_count");
_Static_assert(offsetof(box64_native_module, bridges) == 4 * sizeof(void*),
               "bridges after host_library");
_Static_assert(offsetof(box64_native_module, bridge_count) ==
                   5 * sizeof(void*),
               "bridge_count last");
_Static_assert(offsetof(box64_bridge_entry, name) == 0, "entry name first");
_Static_assert(offsetof(box64_bridge_entry, flags) == 4 * sizeof(void*),
               "entry flags last");

const char* bridge_seam_spike_lookup(const char* soname) {
  return box64_native_module_for_soname(soname);
}

unsigned bridge_seam_spike_count(void) {
  return box64_native_module_count();
}
