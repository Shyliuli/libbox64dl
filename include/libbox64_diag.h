/*
 * Shared diagnostics switch for libbox64dl's own tracing.
 *
 * The embed entry points used to print an unconditional "[box64] ..." line per
 * call.  On a per-call hot path that is a stdio lock plus a write syscall per
 * call, so the traces are now opt-in and default OFF:
 *
 *   - the level is read once from BOX64DL_DIAG ("0" or unset = off) on first
 *     use, then cached in one int; the hot check is a single predictable load;
 *   - an embedder can override it explicitly with box64_diag_set()/get()
 *     (libbox64.h);
 *   - errors never travel through this switch: refusals still record the
 *     thread-local box64_dlerror() slot (see libbox64.h), and signal-route
 *     fault reports still go to stderr unconditionally.
 *
 * This header is internal to the library (not part of the public include set);
 * it exists only so libbox64.c and thunk.c share one switch.
 */
#ifndef LIBBOX64_DIAG_H
#define LIBBOX64_DIAG_H

#include "libbox64.h"

#include <stdio.h>

/* 1 when tracing is enabled, 0 otherwise. First call reads the environment
 * once; subsequent calls are a cached int test. */
int box64_diag_enabled(void);

#define BOX64_DIAG(...)                        \
  do {                                         \
    if (box64_diag_enabled()) printf(__VA_ARGS__); \
  } while (0)

#endif /* LIBBOX64_DIAG_H */
