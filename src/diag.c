/* Shared diagnostics switch (see include/libbox64_diag.h). */
#define _GNU_SOURCE
#include "libbox64_diag.h"

#include <stdlib.h>
#include <string.h>

/* -1 means "BOX64DL_DIAG not consulted yet"; the default when unset is 0
 * (silent). The first use may race between two threads, but both compute the
 * same value from the same environment, so the race is benign and lock-free —
 * a mutex here would put the lock back on the hot path we are removing. */
static int g_diag_level = -1;

static void diag_init_once(void) {
  const char* e = getenv("BOX64DL_DIAG");
  g_diag_level = (e && e[0] && strcmp(e, "0") != 0) ? 1 : 0;
}

int box64_diag_enabled(void) {
  if (__builtin_expect(g_diag_level < 0, 0)) diag_init_once();
  return g_diag_level > 0;
}

/* Explicit override for embedders; takes effect immediately and permanently
 * (it also cancels the lazy environment read). */
BOX64_API void box64_diag_set(int level) { g_diag_level = level; }

BOX64_API int box64_diag_get(void) {
  if (__builtin_expect(g_diag_level < 0, 0)) diag_init_once();
  return g_diag_level;
}
