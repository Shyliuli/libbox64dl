/*
 * collide_guest.c — the same-named x86_64 guest fixture for the H1
 * fail-closed regression in tests/test_bridge_wiring.c.
 *
 * A registered native-module descriptor claims a guest soname. If the engine
 * cannot build the wrapped library from that descriptor (host dlopen failure,
 * out-of-memory, linkmap failure), it must fail the load instead of silently
 * falling through to the emulated path. The hazard is exactly this file: a
 * real x86_64 ELF sitting in the library search directory under the very
 * soname the descriptor claimed. If the engine falls through, the guest loads
 * and its constructor runs — this fixture's constructor drops a marker file so
 * the test can tell "not loaded" from "loaded".
 *
 * Built twice by the Makefile with -DMARKER_NAME=... (one marker for the
 * unclaimed control, one for the claimed/fail-closed case). Linked against
 * guest libc on purpose (no -nostdlib): the marker path goes through the same
 * wrapped-libc resolution every other libc-linked guest fixture uses, so a
 * successful control load proves the fixture is genuinely loadable.
 */
#include <stdio.h>

#ifndef MARKER_NAME
#define MARKER_NAME "seam_guest_ctor_marker"
#endif

__attribute__((constructor)) static void guest_ctor(void) {
  FILE* f = fopen(MARKER_NAME, "w");
  if (!f) return;
  fputs("guest-ctor-ran\n", f);
  fclose(f);
}

/* A symbol the test can resolve after a successful load. */
int seam_collide_marker(void) { return 0x5ea5; }
