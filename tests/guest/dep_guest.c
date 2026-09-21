/*
 * Guest fixture with a real DT_NEEDED dependency on libsig_guest2.so.
 *
 * Used by tests/test_signal_library.c to prove that a library's signal-policy
 * range is dropped at the real image-free point (FreeElfMemory) even when the
 * image is a DEPENDENCY unloaded through a guest my_dlclose of its parent, not
 * a top-level box64_dlclose() handle. The constructor actually calls into the
 * dependency so the loader must bind and keep it alive.
 */
extern long g_sig_hits2(void);

static long g_seen;

__attribute__((constructor)) static void dep_ctor(void) {
  g_seen = g_sig_hits2();
}

long g_dep_seen(void) { return g_seen; }
