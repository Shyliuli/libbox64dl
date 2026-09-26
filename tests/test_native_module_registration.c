/*
 * test_native_module_registration.c — engine-free protocol check of the
 * native-module registration entry (include/libbox64_native_module.h).
 *
 * This drives the REAL src/native_module.c with a plain host compiler: no
 * box64 internals, no Rust facade, no guest, no wrapper table. It pins the
 * descriptor wire shape (so an accidental field reorder cannot silently
 * corrupt the provider's descriptor), the ABI-version refusal, descriptor and
 * bridge-entry validation, and the fixed-capacity, no-registry semantics of
 * the entry.
 *
 * The real-engine half — that a registered descriptor's bridge table is
 * actually consumed by the wrapped-library load path, with ordinary host
 * symbols resolved by dlsym and GOM entries bridged to provider handlers — is
 * tests/test_bridge_wiring.c, linked against the engine archive. The
 * signature -> wrapper lookup is checked there too (it lives in a box64-linked
 * source, so it cannot be in this engine-free TU).
 *
 * Build/run: make check-bridge-registration
 */
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "libbox64_native_module.h"

/* The provider (uninode/native/uninode_native_module.h) declares this exact
 * layout; a field reorder on either side must fail at compile time. */
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
_Static_assert(offsetof(box64_bridge_entry, signature) == sizeof(void*),
               "entry signature after name");
_Static_assert(offsetof(box64_bridge_entry, host_symbol) == 2 * sizeof(void*),
               "entry host_symbol after signature");
_Static_assert(offsetof(box64_bridge_entry, handler) == 3 * sizeof(void*),
               "entry handler after host_symbol");
_Static_assert(offsetof(box64_bridge_entry, flags) == 4 * sizeof(void*),
               "entry flags last");

_Static_assert(BOX64_NATIVE_MODULE_ABI == 2u, "ABI version pinned");
_Static_assert(BOX64_NATIVE_MODULE_MAX >= 2u, "room for the tests below");
_Static_assert(BOX64_BRIDGE_GOM != 0u, "GOM flag is a real bit");

static int g_fails;
static void check(int cond, const char* what) {
  if (cond) {
    printf("[native-module-engine] ok   %s\n", what);
  } else {
    printf("[native-module-engine] FAIL %s\n", what);
    g_fails++;
  }
}

static const char* const sonames_a[] = {"libseam_a.so"};
static const char* const sonames_b[] = {"libseam_b.so", "libseam_b.so.1"};
static const char* const sonames_c[] = {"libseam_c.so"};
static const char* const sonames_d[] = {"libseam_d.so"};

static int g_handler(void) { return 0; }

/* A provider descriptor with a real (if tiny) bridge table: one ordinary entry
 * and one GOM entry, so validation exercises both kinds. */
static const box64_bridge_entry bridges_a[] = {
    {"seam_ordinary", "iFppp", NULL, NULL, 0},
    {"seam_gom", "iFEppp", "seam_gom_host", (void*)&g_handler, BOX64_BRIDGE_GOM},
};
static const box64_bridge_entry bridge_no_name[] = {
    {NULL, "iFppp", NULL, NULL, 0},
};
static const box64_bridge_entry bridge_no_sig[] = {
    {"seam", NULL, NULL, NULL, 0},
};
static const box64_bridge_entry bridge_gom_no_handler[] = {
    {"seam", "iFEppp", NULL, NULL, BOX64_BRIDGE_GOM},
};
/* Two entries for one guest symbol name: the wrapped library would carry two
 * bridges for `seam_dup` and the resolved one would depend on registration
 * order, so the descriptor must be refused as a whole. */
static const box64_bridge_entry bridge_dup_name[] = {
    {"seam_dup", "iFppp", NULL, NULL, 0},
    {"seam_dup", "iFppp", NULL, NULL, BOX64_BRIDGE_WEAK},
};

static const box64_native_module g_module_a = {
    "seam_a", sonames_a, 1, NULL, bridges_a,
    sizeof(bridges_a) / sizeof(bridges_a[0])};
static const box64_native_module g_module_b = {"seam_b", sonames_b, 2, NULL,
                                               NULL, 0};
static const box64_native_module g_module_c = {"seam_c", sonames_c, 1,
                                               "libseam_host.so", NULL, 0};
static const box64_native_module g_module_d = {"seam_d", sonames_d, 1, NULL,
                                               NULL, 0};

int main(void) {
  const box64_native_module dup_a = {"seam_a", sonames_c, 1, NULL, NULL, 0};
  const box64_native_module no_name = {NULL, sonames_a, 1, NULL, NULL, 0};
  const box64_native_module empty_name = {"", sonames_a, 1, NULL, NULL, 0};
  const box64_native_module no_soname_table = {"bad", NULL, 2, NULL, NULL, 0};
  const box64_native_module no_bridge_table = {"bad", NULL, 0, NULL, NULL, 3};
  const box64_native_module bad_entry =
      {"bad", sonames_a, 1, NULL, bridge_no_name, 1};
  const box64_native_module bad_sig =
      {"bad2", sonames_a, 1, NULL, bridge_no_sig, 1};
  const box64_native_module bad_gom =
      {"bad3", sonames_a, 1, NULL, bridge_gom_no_handler, 1};
  const box64_native_module bad_dup =
      {"bad4", sonames_a, 1, NULL, bridge_dup_name, 2};

  check(box64_native_module_count() == 0, "fresh table is empty");

  /* ABI version is part of the contract, not advisory. */
  check(box64_register_native_module(&g_module_a, BOX64_NATIVE_MODULE_ABI + 1u) !=
            0,
        "unknown ABI version refused");
  check(box64_native_module_count() == 0,
        "an ABI refusal records nothing");

  /* Malformed descriptors and malformed bridge entries never enter the table. */
  check(box64_register_native_module(NULL, BOX64_NATIVE_MODULE_ABI) != 0,
        "NULL descriptor refused");
  check(box64_register_native_module(&no_name, BOX64_NATIVE_MODULE_ABI) != 0,
        "nameless descriptor refused");
  check(box64_register_native_module(&empty_name, BOX64_NATIVE_MODULE_ABI) != 0,
        "empty module name refused");
  check(box64_register_native_module(&no_soname_table,
                                     BOX64_NATIVE_MODULE_ABI) != 0,
        "soname count without a table refused");
  check(box64_register_native_module(&no_bridge_table,
                                     BOX64_NATIVE_MODULE_ABI) != 0,
        "bridge count without a table refused");
  check(box64_register_native_module(&bad_entry, BOX64_NATIVE_MODULE_ABI) != 0,
        "bridge entry without a name refused");
  check(box64_register_native_module(&bad_sig, BOX64_NATIVE_MODULE_ABI) != 0,
        "bridge entry without a signature refused");
  check(box64_register_native_module(&bad_gom, BOX64_NATIVE_MODULE_ABI) != 0,
        "GOM bridge entry without a handler refused");
  check(box64_register_native_module(&bad_dup, BOX64_NATIVE_MODULE_ABI) != 0,
        "duplicate bridge entry name refused");
  check(box64_native_module_count() == 0,
        "rejected descriptors never reached the table");

  /* g_module_a carries a real bridge table and a NULL host_library (the
   * running process); both are recorded. */
  check(box64_register_native_module(&g_module_a, BOX64_NATIVE_MODULE_ABI) == 0,
        "descriptor with a bridge table accepted");
  check(box64_native_module_count() == 1, "one module recorded");
  check(box64_native_module_for_soname("libseam_a.so") != NULL &&
            strcmp(box64_native_module_for_soname("libseam_a.so"), "seam_a") ==
                0,
        "registered soname resolves to its module name");
  check(box64_native_module_for_soname("libseam_unknown.so") == NULL,
        "unregistered soname resolves to NULL (fail-closed)");
  check(box64_native_module_for_soname(NULL) == NULL,
        "NULL soname resolves to NULL");
  check(box64_native_module_for_name("seam_a") == &g_module_a,
        "module name resolves to the exact descriptor (bridge table intact)");
  check(box64_native_module_for_name(NULL) == NULL,
        "NULL module name resolves to NULL");

  /* One entry per module name: re-registering is refused, whatever the
   * descriptor says. */
  check(box64_register_native_module(&dup_a, BOX64_NATIVE_MODULE_ABI) != 0,
        "duplicate module name refused");
  check(box64_native_module_count() == 1, "duplicate did not add an entry");

  /* Fill the fixed table, then prove the next distinct module is refused. */
  check(box64_register_native_module(&g_module_b, BOX64_NATIVE_MODULE_ABI) == 0,
        "second module registered");
  check(box64_register_native_module(&g_module_c, BOX64_NATIVE_MODULE_ABI) == 0,
        "third module registered");
  check(box64_register_native_module(&g_module_d, BOX64_NATIVE_MODULE_ABI) == 0,
        "fourth module registered");
  check(box64_native_module_count() == BOX64_NATIVE_MODULE_MAX,
        "table is full at the fixed capacity");
  {
    static const char* const sonames_e[] = {"libseam_e.so"};
    const box64_native_module e = {"seam_e", sonames_e, 1, NULL, NULL, 0};
    check(box64_register_native_module(&e, BOX64_NATIVE_MODULE_ABI) != 0,
          "capacity overflow refused (no silent growth)");
  }

  /* Every soname of a multi-soname module resolves; the table is exact-match. */
  check(box64_native_module_for_soname("libseam_b.so") != NULL &&
            strcmp(box64_native_module_for_soname("libseam_b.so"), "seam_b") ==
                0,
        "first soname of a multi-soname module resolves");
  check(box64_native_module_for_soname("libseam_b.so.1") != NULL,
        "second soname of a multi-soname module resolves");
  check(box64_native_module_for_soname("libseam_b.so.2") == NULL,
        "lookup is exact, not a prefix match");

  /* A host_library is carried verbatim (module_c) and NULL is legal. */
  check(box64_native_module_for_name("seam_c") != NULL &&
            strcmp(box64_native_module_for_name("seam_c")->host_library,
                   "libseam_host.so") == 0,
        "host_library is part of the recorded descriptor");

  if (g_fails) {
    printf("[native-module-engine] %d FAILURES\n", g_fails);
    return 1;
  }
  printf("[native-module-engine] all checks passed\n");
  return 0;
}
