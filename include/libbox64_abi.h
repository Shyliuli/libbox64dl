/*
 * libbox64_abi.h — the engine register-file ABI types and capacity constants,
 * defined ONCE.
 *
 * Pure type header (no function declarations). include/libbox64.h includes it
 * and is the single home of the full box64_call_plan() contract; the layers
 * below name these same types rather than mirroring them:
 *   - the box64 engine (libbox64.c / dynarec's DynaCall_abi seam);
 *   - unilib's generated-stub seam: include/unilib.h aliases unilib_sigplan /
 *     unilib_fpr / unilib_results to these types and maps UNILIB_SIGPLAN to
 *     BOX64_SIGPLAN, so src/gencall.c only adapts the error policy;
 *   - uninode's ELF generator, which emits box64_sigplan_t / BOX64_SIGPLAN
 *     through include/libbox64.h and so needs no unilib dependency.
 *
 * Mask bit layout (see box64_call_plan() for the full semantics):
 *   gpr_mask bit i -> argument GPR position i (0=rdi, 1=rsi, 2=rdx, 3=rcx,
 *                     4=r8, 5=r9)
 *   fpr_mask bit i -> argument SSE position i (0=xmm0 .. 7=xmm7)
 *   ret_mask bit 0 -> rax   bit 1 -> rdx   bit 2 -> xmm0   bit 3 -> xmm1
 *   stack_len      -> outgoing stack bytes; 0 = no stack blob, and the engine
 *                     rounds any other count up to 16
 */
#ifndef LIBBOX64_ABI_H
#define LIBBOX64_ABI_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Engine register-file capacity. These are engine facts, not ABI decisions: a
 * backend maps its calling convention onto this file and must not claim more
 * positions than the engine provides. unilib's src/abi.h aliases these names. */
#define BOX64_ENGINE_ARG_GPRS 6 /* argument GPRs: rdi,rsi,rdx,rcx,r8,r9 */
#define BOX64_ENGINE_ARG_FPRS 8 /* argument SSE: xmm0..xmm7 */
#define BOX64_ENGINE_RET_GPRS 2 /* return GPRs: rax,rdx */
#define BOX64_ENGINE_RET_FPRS 2 /* return SSE: xmm0,xmm1 */

typedef struct box64_sigplan {
  uint16_t gpr_mask;  /* argument GPR positions the callee reads */
  uint16_t fpr_mask;  /* argument SSE positions the callee reads */
  uint16_t ret_mask;  /* bit0 rax, bit1 rdx, bit2 xmm0, bit3 xmm1 */
  uint16_t stack_len; /* outgoing stack bytes; engine rounds up to 16 */
} box64_sigplan_t;

/* One SSE register value: lo/hi are sse_regs_t.sq[0]/sq[1] (all 16 bytes). */
typedef struct box64_fpr {
  uint64_t lo;
  uint64_t hi;
} box64_fpr_t;

typedef struct box64_call_results {
  uint64_t rax;
  uint64_t rdx;
  box64_fpr_t xmm0;
  box64_fpr_t xmm1;
} box64_call_results_t;

/* Compile-time initialiser for a static plan: BOX64_SIGPLAN(gpr, fpr, ret, stack) */
#define BOX64_SIGPLAN(gpr, fpr, ret, stack) \
  { (uint16_t)(gpr), (uint16_t)(fpr), (uint16_t)(ret), (uint16_t)(stack) }

#ifdef __cplusplus
}
#endif

#endif /* LIBBOX64_ABI_H */
