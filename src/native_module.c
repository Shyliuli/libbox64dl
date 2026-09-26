/*
 * native_module.c — engine side of the explicit native-module seam.
 *
 * See include/libbox64_native_module.h for the contract. This translation unit
 * is deliberately engine-free (no box64 internals, no Rust facade): that is
 * what lets it be unit-tested with a plain host compiler and what keeps the
 * descriptor table independent of the vendored engine.
 *
 * The table is fixed-size and holds caller-owned identity pointers. There is
 * no directory scan, no config file, no dynamic loading and no generic
 * registry: uninode names its one module explicitly. The wrapped-library load
 * path (vendored librarian/library.c) consults these lookups to build a
 * runtime wrapped library from the descriptor's bridge table; the engine's
 * signature->wrapper lookup lives in native_module_wrappers.c.
 *
 * COLD PATH: registration is a pre-guest-load configuration operation. No lock
 * is taken here; see the config contract in the header.
 */
#include "libbox64_native_module.h"

#include <string.h>

typedef struct native_module_slot {
  int used;
  const box64_native_module* module;
} native_module_slot;

static native_module_slot g_slots[BOX64_NATIVE_MODULE_MAX];

/* A descriptor is well-formed when its identity is complete, every count that
 * is non-zero has a matching table, and every bridge entry is usable. The
 * provider also validates, but the engine refuses directly so a hand-written
 * descriptor cannot be misread. */
static int bridge_entry_ok(const box64_bridge_entry* entry) {
  if (!entry) return 0;
  if (!entry->name || entry->name[0] == '\0') return 0;
  if (!entry->signature || entry->signature[0] == '\0') return 0;
  if ((entry->flags & BOX64_BRIDGE_GOM) && !entry->handler) return 0;
  return 1;
}

static int descriptor_ok(const box64_native_module* module) {
  size_t i;
  if (!module) return 0;
  if (!module->name || module->name[0] == '\0') return 0;
  if (module->soname_count && !module->sonames) return 0;
  if (module->bridge_count && !module->bridges) return 0;
  for (i = 0; i < module->bridge_count; ++i)
    if (!bridge_entry_ok(&module->bridges[i])) return 0;
  /* One bridge per guest symbol name. A duplicate would make the wrapped
   * library carry two bridges for one name, so the symbol the guest resolves
   * would depend on which entry the engine happened to register first.
   * Refuse the descriptor instead of picking silently. */
  for (i = 0; i < module->bridge_count; ++i) {
    size_t j;
    for (j = i + 1; j < module->bridge_count; ++j)
      if (strcmp(module->bridges[i].name, module->bridges[j].name) == 0)
        return 0;
  }
  return 1;
}

int box64_register_native_module(const box64_native_module* module,
                                 unsigned abi_version) {
  unsigned i;
  if (abi_version != BOX64_NATIVE_MODULE_ABI) return -1;
  if (!descriptor_ok(module)) return -1;
  for (i = 0; i < BOX64_NATIVE_MODULE_MAX; ++i) {
    if (g_slots[i].used && strcmp(g_slots[i].module->name, module->name) == 0)
      return -1; /* one entry per module name; re-registration is refused */
  }
  for (i = 0; i < BOX64_NATIVE_MODULE_MAX; ++i) {
    if (!g_slots[i].used) {
      g_slots[i].used = 1;
      g_slots[i].module = module;
      return 0;
    }
  }
  return -1; /* fixed table full */
}

const char* box64_native_module_for_soname(const char* soname) {
  unsigned i;
  size_t j;
  if (!soname) return NULL;
  for (i = 0; i < BOX64_NATIVE_MODULE_MAX; ++i) {
    const box64_native_module* module;
    if (!g_slots[i].used) continue;
    module = g_slots[i].module;
    for (j = 0; j < module->soname_count; ++j) {
      if (module->sonames[j] && strcmp(module->sonames[j], soname) == 0)
        return module->name;
    }
  }
  return NULL;
}

const box64_native_module* box64_native_module_for_name(const char* name) {
  unsigned i;
  if (!name) return NULL;
  for (i = 0; i < BOX64_NATIVE_MODULE_MAX; ++i) {
    if (g_slots[i].used && strcmp(g_slots[i].module->name, name) == 0)
      return g_slots[i].module;
  }
  return NULL;
}

unsigned box64_native_module_count(void) {
  unsigned i;
  unsigned n = 0;
  for (i = 0; i < BOX64_NATIVE_MODULE_MAX; ++i)
    if (g_slots[i].used) ++n;
  return n;
}
