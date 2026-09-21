/*
 * Signal-handling policy seam for libbox64dl (internal; grafted into the engine
 * build together with src/signal_policy.c and wired up by patches/0005).
 *
 * WHY THIS EXISTS
 * ---------------
 * Every top-level guest call enters EmuRun() (dynarec/dynarec.c), which arms a
 * signal-recovery jmpbuf with `SigSetJmp(emu->jmpbuf, savesigs)`. On glibc,
 * saving the signal mask (savesigs != 0) makes __sigjmp_save() issue an
 * rt_sigprocmask syscall per call just to snapshot a mask the no-signal path
 * never reads. The policy chooses how that is done:
 *
 *   COMPAT  SigSetJmp(buf, 1): the saved mask M0 is restored by siglongjmp.
 *           Byte-for-byte what the engine always did.
 *   FAST    SigSetJmp(buf, 0) (no syscall). On every signal-driven resume the
 *           handler restores the mask the kernel recorded in the delivered
 *           signal's ucontext (M1, the pre-delivery mask).
 *
 * FAST is the default. It is sound for box64's own handlers (installed with
 * SA_NODEFER and an empty sa_mask) and for a guest-installed handler that adds
 * its own sa_mask bits: restoring the kernel ucontext mask removes exactly
 * those. It can differ from COMPAT's entry mask M0 because the guest, the host
 * or the engine may change the mask between EmuRun entry and the signal.
 *
 * PER-FRAME, NOT GLOBAL
 * ---------------------
 * The policy is not one process-wide switch any more. DynaCallCore resolves a
 * policy per *library entry scope* from the target address and publishes it on
 * the emu as emu->requested_call_policy; EmuRun arms its frame with that policy
 * and records it as emu->active_jmpbuf_policy. A nested EmuRun whose requested
 * policy differs from the active frame's forces a NEW frame, so the frame the
 * handler resumes into always carries the policy the handler must restore
 * under. When the policies agree, the original nested-sharing behaviour is
 * kept unchanged.
 *
 * A "scope" is the target library of the current host entry, not every
 * instruction: an internal guest->guest call does not re-enter EmuRun and does
 * not re-select a policy. This is deliberate and documented as not being
 * absolute isolation in include/libbox64.h.
 */

#ifndef LIBBOX64_SIGNAL_H
#define LIBBOX64_SIGNAL_H

#include <stdint.h>

/* Policy values; the public enum in include/libbox64.h has the same numbers. */
#define BOX64DL_POLICY_COMPAT 0
#define BOX64DL_POLICY_FAST 1
/* emu->requested_call_policy sentinel: resolve the policy from the entry PC
 * (covers constructors, DynaCall from wrapped code, anything without a host
 * entry that already set a scope). */
#define BOX64DL_POLICY_INHERIT (-1)

/* ------------------------------------------------------------------------ *
 * Published global default (read on the hot path, never locked).
 * ------------------------------------------------------------------------ */

/* 1 only when per-call diagnostics counters are enabled
 * (BOX64DL_SIGNAL_STATS or box64_signal_stats_enable()). Default 0. */
extern int box64dl_sig_dbg_on;

/* Route-fault tracing (src/signal_route.c): resolved once at publish so the
 * signal path never calls getenv. */
extern int box64dl_route_trace;

/* Record a caller-selected global default before init. Returns 0, or -1 when
 * the value is out of range or the default was already published. */
int box64dl_signal_policy_request(int policy);

/* Resolve and publish the global default: explicit request >
 * BOX64DL_SIGNAL_POLICY ("compat"|"fast") > FAST. Safe to call more than once.
 * Returns 0, or -1 when the environment selected an unknown value or the
 * unsupported "none". */
int box64dl_signal_policy_publish(void);

/* The published global default, FAST before publish. */
int box64dl_signal_policy_mode(void);

/* Static reason string for the most recent publish refusal (never NULL). */
const char* box64dl_signal_policy_refusal(void);

/* ------------------------------------------------------------------------ *
 * Per-entry resolution (hot path).
 * ------------------------------------------------------------------------ */

/* Policy for a guest entry address. This is the ONE hot-path resolver:
 *   - no library overrides configured: returns the global default after a
 *     single relaxed load (no cache, no scan);
 *   - overrides configured: per-thread O(1) direct-mapped cache, refilled by a
 *     small linear scan of the registered library ranges on a miss.
 * It never calls FindElfAddress (which is a linear walk over every elf) and
 * never takes a lock. Ranges are plain values, so a cached entry cannot dangle
 * across dlclose; a config/dlclose epoch bump invalidates every thread's cache
 * instead.
 *
 * CONFIG CONTRACT: the range table is written only at config/load/unload time.
 * Callers must serialize that with guest execution (including any signal
 * handler that can re-enter the engine): the atomic fields avoid a data race
 * but do NOT provide a coherent multi-field snapshot, so a concurrent
 * configuration is not supported. With configuration quiescent the resolver is
 * a lock-free, O(1) read.
 *
 * ADDRESSES OUTSIDE EVERY IMAGE resolve to the global default. Host code and
 * box64's own bridge shims are not a guest library, so they get the default
 * policy; a call the guest makes through such a shim either does not re-enter
 * EmuRun (native dispatch) or re-enters with its own target PC. No per-bridge
 * shim prebinding is needed for the EmuRun frame path. */
int box64dl_signal_policy_for_addr(uintptr_t addr);

/* 1 when at least one library range override is registered. */
int box64dl_signal_lib_rules_active(void);

/* Register/update (lo,hi) -> policy and publish it to every thread. `lo/hi` are
 * host addresses of the library image; policy must be COMPAT or FAST. Setting
 * the policy back to the global default clears the override. Returns 0, or -1
 * when the arguments are invalid or the bounded table is full (the caller must
 * report the failure; a silently dropped override would make a configuration
 * that looks applied do nothing). */
int box64dl_signal_lib_range_set(uintptr_t lo, uintptr_t hi, int policy);

/* Drop the override registered for `lo` (used on dlclose). Safe when absent. */
void box64dl_signal_lib_range_clear(uintptr_t lo);

/* Match one library path against BOX64DL_SIGNAL_LIBRARY_RULES. Returns the
 * policy, or -1 when no rule matches. Exact matching only:
 *   - a MATCH containing '/' is compared to the full path (and its realpath);
 *   - any other MATCH is compared to the exact final path component.
 * No substring, glob or prefix matching. The environment is parsed once. */
int box64dl_signal_lib_rule_lookup(const char* path);

/* Bump the invalidation epoch (called on dlclose/config so no thread keeps a
 * stale cached policy). */
void box64dl_signal_lib_rules_epoch(void);

/* ------------------------------------------------------------------------ *
 * Slow path.
 * ------------------------------------------------------------------------ */

/* Restore the signal mask on a signal-driven resume. `active_policy` is the
 * policy of the frame the handler is about to siglongjmp into
 * (emu->active_jmpbuf_policy), NOT a global: a COMPAT frame is a no-op because
 * siglongjmp restores the saved mask M0 itself, while a FAST frame restores
 * the delivered ucontext's mask M1. NULL `ucntx` is a defensive no-op. */
void box64dl_signal_mask_restore(int active_policy, const void* ucntx);

/* ------------------------------------------------------------------------ *
 * Diagnostics.
 * ------------------------------------------------------------------------ */

/* Counters. saves/skips count actual SigSetJmp() invocations per thread
 * (gated by dbg_on, default off). restores counts fast-frame slow-path
 * restores process-wide; it is always maintained and uses a relaxed atomic
 * because it is incremented from a signal handler (never on the call path). */
void box64dl_sig_dbg_note(int saved);
unsigned long box64dl_sig_dbg_saves(void);
unsigned long box64dl_sig_dbg_skips(void);
unsigned long box64dl_sig_dbg_restores(void);
int box64dl_sig_dbg_publishes(void);

#endif /* LIBBOX64_SIGNAL_H */
