/*
 * libbox64_abi.h — the engine register-file ABI types and capacity constants,
 * defined ONCE.
 *
 * Type header plus the one inline predicate that validates a descriptor
 * (box64_call_desc_validate). include/libbox64.h includes it and is the single
 * home of the full box64_call_desc() contract; the consumers below name these
 * same types rather than mirroring them:
 *   - the box64 engine (libbox64.c / dynarec's DynaCall_abi seam);
 *   - unilib's generated stubs, which alias only unilib_fpr to box64_fpr_t and
 *     otherwise name box64_call_simple()/box64_call_desc() and the engine's
 *     plan/result types directly (the old unilib_sigplan / unilib_results
 *     aliases and the UNILIB_SIGPLAN macro were retired);
 *   - uninode's generated bridges, which call box64_call_simple() through
 *     libbox64.h and assert the retired box64_sigplan_t / BOX64_SIGPLAN
 *     spelling does not reappear.
 *
 * Mask bit layout (see box64_call_desc() for the full semantics):
 *   gpr_mask bit i -> argument GPR position i (0=rdi, 1=rsi, 2=rdx, 3=rcx,
 *                     4=r8, 5=r9)
 *   fpr_mask bit i -> argument SSE position i (0=xmm0 .. 7=xmm7)
 *   ret_mask bit 0 -> rax   bit 1 -> rdx   bit 2 -> xmm0   bit 3 -> xmm1
 *   stack_len      -> outgoing stack bytes; 0 = no stack blob, and the engine
 *                     rounds any other count up to 16
 */
#ifndef LIBBOX64_ABI_H
#define LIBBOX64_ABI_H

#include <stddef.h>
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

/* Explicit result status for the unified call entries.  A non-OK status means
 * the guest did not execute and any output object must be ignored. */
typedef enum box64_call_status {
  BOX64_CALL_OK             = 0,
  BOX64_CALL_BAD_ARGUMENT   = -1,
  BOX64_CALL_NOT_READY      = -2,
  BOX64_CALL_BAD_LAYOUT     = -3,
  BOX64_CALL_NO_EMU         = -4,
  BOX64_CALL_ENGINE_REFUSED = -5
} box64_call_status_t;

/*
 * flags bit 0 — BOX64_CALL_FLAG_FULL_REGS
 *   Selects the engine's generic DynaCall entry instead of the known-ABI
 *   DynaCall_abi entry. On a normal return the generic entry writes the
 *   caller's pre-call RDI/RSI/RBX/RBP back into the register file; DynaCall_abi
 *   skips exactly that restore group and leaves those four registers as the
 *   call left them. RSP, RIP, eflags, deferred flags/operands and the longjmp
 *   rule are identical in both modes, and neither mode restores any other
 *   register — in particular the Win64 volatile RCX/RDX/R8/R9 are not covered
 *   by either.
 *
 *   This is a caller-visible postcondition, not a per-ABI optimisation switch:
 *   keep it whenever the caller needs that four-register group preserved across
 *   the call (the removed register-file entries did); clearing it is only safe
 *   when the caller has established those registers are dead afterwards — for
 *   example a SysV64 call whose RDI/RSI results are not read, or a conforming
 *   Win64 callee that already preserved its non-volatile RDI/RSI. Do not clear
 *   it by applying one ABI's clobber rules to the other: Win64 makes RDI/RSI
 *   callee-saved but RCX/RDX/R8/R9 volatile, which is the opposite of SysV64,
 *   and this flag restores RDI/RSI for both.
 *
 * flags bit 1 — BOX64_CALL_FLAG_FORCE_FRAME
 *   Anchors a call frame even when stack_len is zero. A non-zero stack_len
 *   always anchors a frame regardless of this bit.
 */
#define BOX64_CALL_FLAG_FULL_REGS   (1u << 0)
#define BOX64_CALL_FLAG_FORCE_FRAME (1u << 1)

#define BOX64_CALL_RET_RAX  (1u << 0)
#define BOX64_CALL_RET_RDX  (1u << 1)
#define BOX64_CALL_RET_XMM0 (1u << 2)
#define BOX64_CALL_RET_XMM1 (1u << 3)

/* Descriptor addresses are deliberately ABI-neutral 64-bit values. */
#define BOX64_PTR_TO_U64(p) ((uint64_t)(uintptr_t)(p))

typedef struct box64_call_desc {
  uint32_t flags;      /* only FULL_REGS and FORCE_FRAME are accepted */
  uint16_t gpr_mask;   /* physical argument GPR positions 0..5 */
  uint16_t fpr_mask;   /* physical argument XMM positions 0..7 */
  uint16_t ret_mask;   /* RAX, RDX, XMM0 and XMM1 return bits */
  uint16_t reserved0;  /* must be zero */
  uint32_t stack_len;  /* zero permits a zero stack pointer */
  uint32_t reserved1;  /* must be zero */
  uint64_t gprs;       /* const uint64_t[6], fixed physical indices */
  uint64_t fprs;       /* const box64_fpr_t[8], fixed physical indices */
  uint64_t stack;      /* const void*, stack_len bytes */
} box64_call_desc_t;

#ifdef __cplusplus
#define BOX64_ABI_STATIC_ASSERT static_assert
#else
#define BOX64_ABI_STATIC_ASSERT _Static_assert
#endif
BOX64_ABI_STATIC_ASSERT(sizeof(box64_call_desc_t) == 48,
                        "box64_call_desc_t size must remain 48 bytes");
BOX64_ABI_STATIC_ASSERT(offsetof(box64_call_desc_t, flags) == 0,
                        "box64_call_desc_t.flags offset");
BOX64_ABI_STATIC_ASSERT(offsetof(box64_call_desc_t, gpr_mask) == 4,
                        "box64_call_desc_t.gpr_mask offset");
BOX64_ABI_STATIC_ASSERT(offsetof(box64_call_desc_t, fpr_mask) == 6,
                        "box64_call_desc_t.fpr_mask offset");
BOX64_ABI_STATIC_ASSERT(offsetof(box64_call_desc_t, ret_mask) == 8,
                        "box64_call_desc_t.ret_mask offset");
BOX64_ABI_STATIC_ASSERT(offsetof(box64_call_desc_t, reserved0) == 10,
                        "box64_call_desc_t.reserved0 offset");
BOX64_ABI_STATIC_ASSERT(offsetof(box64_call_desc_t, stack_len) == 12,
                        "box64_call_desc_t.stack_len offset");
BOX64_ABI_STATIC_ASSERT(offsetof(box64_call_desc_t, reserved1) == 16,
                        "box64_call_desc_t.reserved1 offset");
BOX64_ABI_STATIC_ASSERT(offsetof(box64_call_desc_t, gprs) == 24,
                        "box64_call_desc_t.gprs offset");
BOX64_ABI_STATIC_ASSERT(offsetof(box64_call_desc_t, fprs) == 32,
                        "box64_call_desc_t.fprs offset");
BOX64_ABI_STATIC_ASSERT(offsetof(box64_call_desc_t, stack) == 40,
                        "box64_call_desc_t.stack offset");
#undef BOX64_ABI_STATIC_ASSERT

/*
 * The one shared descriptor rule, used by every forward-call entry that accepts
 * a box64_call_desc_t: libbox64dl's box64_call_desc() and winedl's
 * winedl_call_desc(). It validates exactly the fields the engine consumes —
 * flags, both reserved words, the three physical mask bounds and the pointer
 * presence rules — and nothing else, so no caller runs a partially different
 * check set and no third execution entry is introduced.
 *
 * Returns BOX64_CALL_OK when the descriptor may be executed.
 *   BOX64_CALL_BAD_LAYOUT   unknown flag bit, non-zero reserved word, or a mask
 *                           bit outside the engine register file
 *                           (gpr > 0x3f, fpr > 0xff, ret > 0x0f).
 *   BOX64_CALL_BAD_ARGUMENT `desc` is NULL, or a set mask bit names a NULL
 *                           gpr/fpr array or a non-empty stack names NULL.
 *
 * `reason` is optional (pass NULL to ignore it). On failure it receives a
 * static string naming the first failing check, so each entry can prefix its
 * own diagnostic without duplicating the rule; on success it is set to NULL.
 * The function is dependency-free and side-effect-free: it touches no engine
 * state and writes nothing through the descriptor.
 */
static inline box64_call_status_t
box64_call_desc_validate(const box64_call_desc_t* desc, const char** reason) {
  if (!desc) {
    if (reason) *reason = "NULL descriptor";
    return BOX64_CALL_BAD_ARGUMENT;
  }
  if (desc->flags & ~(BOX64_CALL_FLAG_FULL_REGS | BOX64_CALL_FLAG_FORCE_FRAME)) {
    if (reason) *reason = "unknown flags";
    return BOX64_CALL_BAD_LAYOUT;
  }
  if (desc->reserved0 || desc->reserved1) {
    if (reason) *reason = "reserved field is non-zero";
    return BOX64_CALL_BAD_LAYOUT;
  }
  if (desc->gpr_mask & (uint16_t)~0x3Fu) {
    if (reason) *reason = "gpr_mask outside 0..5";
    return BOX64_CALL_BAD_LAYOUT;
  }
  if (desc->fpr_mask & (uint16_t)~0xFFu) {
    if (reason) *reason = "fpr_mask outside 0..7";
    return BOX64_CALL_BAD_LAYOUT;
  }
  if (desc->ret_mask & (uint16_t)~0x0Fu) {
    if (reason) *reason = "ret_mask outside 0..3";
    return BOX64_CALL_BAD_LAYOUT;
  }
  if ((desc->gpr_mask && !desc->gprs) || (desc->fpr_mask && !desc->fprs) ||
      (desc->stack_len && !desc->stack)) {
    if (reason) *reason = "mask names a NULL value array";
    return BOX64_CALL_BAD_ARGUMENT;
  }
  if (reason) *reason = NULL;
  return BOX64_CALL_OK;
}

/* Compile-time initialiser for a static plan: BOX64_SIGPLAN(gpr, fpr, ret, stack) */
#define BOX64_SIGPLAN(gpr, fpr, ret, stack) \
  { (uint16_t)(gpr), (uint16_t)(fpr), (uint16_t)(ret), (uint16_t)(stack) }

#ifdef __cplusplus
}
#endif

#endif /* LIBBOX64_ABI_H */
