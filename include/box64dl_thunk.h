/*
 * Runtime-generated host trampolines.
 *
 * Problem this solves: a guest function pointer cannot be handed to host code
 * that will call it — the host would jump straight into x86_64 bytes. The usual
 * workaround is a table of statically generated thunks, which caps how many
 * distinct guest callbacks can ever be bridged. This allocates them on demand
 * instead, so the only limit is memory.
 *
 * Calling a thunk loads data0 into $t0 and data1 into $t1, then tail-calls
 * `target` with the argument registers untouched. `target` is normally a small
 * assembly shim that shifts the arguments up and prepends $t0.
 */
#ifndef BOX64DL_THUNK_H
#define BOX64DL_THUNK_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Existing thunk for (target, data0), or a new one. NULL if out of memory.
 * Deduplicating on data0 is what keeps one guest function to one thunk. */
void* box64dl_thunk_get(void* target, uint64_t data0, uint64_t data1);

/* Existing thunk for (target, data0), or NULL. Never allocates. */
void* box64dl_thunk_peek(void* target, uint64_t data0);

/* data0 the thunk was created with, or 0 if it is not one of ours. */
uint64_t box64dl_thunk_data0(void* thunk);

void box64dl_thunk_release(void* thunk);

/* Release every thunk whose data0 falls in [lo, hi). Call this when unloading a
 * guest module: thunks into unmapped code are worse than leaked ones. Returns
 * how many were freed. */
size_t box64dl_thunk_release_range(uint64_t lo, uint64_t hi);

/* Live thunk count, for diagnostics. */
size_t box64dl_thunk_count(void);

#ifdef __cplusplus
}
#endif

#endif
