#ifndef BOX64_TEST_CALL_COMPAT_H
#define BOX64_TEST_CALL_COMPAT_H

/* Test-only adapters for exercising the historical register-file/plan
 * semantics after their public entry points were removed.  Every adapter
 * builds the unified descriptor and therefore tests the production API rather
 * than keeping compatibility symbols in the library. */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static uint64_t test_desc_regs(void* sym, uint64_t* gregs, uint64_t* fregs) {
  box64_fpr_t fprs[8] = {{0}};
  box64_call_desc_t d;
  box64_call_results_t out;
  size_t i;
  if (!gregs || !fregs) return 0;
  for (i = 0; i < 8; ++i) memcpy(&fprs[i].lo, &fregs[i], sizeof(uint64_t));
  memset(&d, 0, sizeof(d));
  d.flags = BOX64_CALL_FLAG_FULL_REGS;
  d.gpr_mask = 0x3f;
  d.fpr_mask = 0xff;
  d.ret_mask = BOX64_CALL_RET_RAX | BOX64_CALL_RET_RDX |
               BOX64_CALL_RET_XMM0 | BOX64_CALL_RET_XMM1;
  d.gprs = BOX64_PTR_TO_U64(gregs);
  d.fprs = BOX64_PTR_TO_U64(fprs);
  if (box64_call_desc(sym, &d, &out) != BOX64_CALL_OK) return 0;
  gregs[0] = out.rax;
  gregs[1] = out.rdx;
  memcpy(&fregs[0], &out.xmm0.lo, sizeof(uint64_t));
  memcpy(&fregs[1], &out.xmm1.lo, sizeof(uint64_t));
  return out.rax;
}

static uint64_t test_desc_blob(void* sym, uint64_t* gregs,
                                    uint64_t* fregs, const void* stack,
                                    size_t stack_len) {
  box64_fpr_t fprs[8] = {{0}};
  box64_call_desc_t d;
  box64_call_results_t out;
  size_t i;
  if (!gregs || !fregs || stack_len > UINT32_MAX) return 0;
  for (i = 0; i < 8; ++i) memcpy(&fprs[i].lo, &fregs[i], sizeof(uint64_t));
  memset(&d, 0, sizeof(d));
  d.flags = BOX64_CALL_FLAG_FULL_REGS | BOX64_CALL_FLAG_FORCE_FRAME;
  d.gpr_mask = 0x3f;
  d.fpr_mask = 0xff;
  d.ret_mask = BOX64_CALL_RET_RAX | BOX64_CALL_RET_RDX |
               BOX64_CALL_RET_XMM0 | BOX64_CALL_RET_XMM1;
  d.stack_len = (uint32_t)stack_len;
  d.gprs = BOX64_PTR_TO_U64(gregs);
  d.fprs = BOX64_PTR_TO_U64(fprs);
  d.stack = BOX64_PTR_TO_U64(stack);
  if (box64_call_desc(sym, &d, &out) != BOX64_CALL_OK) return 0;
  gregs[0] = out.rax;
  gregs[1] = out.rdx;
  memcpy(&fregs[0], &out.xmm0.lo, sizeof(uint64_t));
  memcpy(&fregs[1], &out.xmm1.lo, sizeof(uint64_t));
  return out.rax;
}

static uint64_t test_desc_plan(void* sym, const box64_sigplan_t* plan,
                               const uint64_t* gprs, const box64_fpr_t* fprs,
                               const void* stack, box64_call_results_t* out) {
  box64_call_desc_t d;
  box64_call_results_t result;
  box64_call_status_t status;
  if (!plan) {
    (void)box64_call_desc(sym, NULL, NULL);
    return 0;
  }
  memset(&d, 0, sizeof(d));
  d.flags = plan->stack_len ? BOX64_CALL_FLAG_FORCE_FRAME : 0;
  d.gpr_mask = plan->gpr_mask;
  d.fpr_mask = plan->fpr_mask;
  d.ret_mask = plan->ret_mask | BOX64_CALL_RET_RAX;
  d.stack_len = plan->stack_len;
  d.gprs = BOX64_PTR_TO_U64(gprs);
  d.fprs = BOX64_PTR_TO_U64(fprs);
  d.stack = BOX64_PTR_TO_U64(stack);
  status = box64_call_desc(sym, &d, &result);
  if (status != BOX64_CALL_OK) return 0;
  if (out) {
    if (plan->ret_mask & BOX64_CALL_RET_RAX) out->rax = result.rax;
    if (plan->ret_mask & BOX64_CALL_RET_RDX) out->rdx = result.rdx;
    if (plan->ret_mask & BOX64_CALL_RET_XMM0) out->xmm0 = result.xmm0;
    if (plan->ret_mask & BOX64_CALL_RET_XMM1) out->xmm1 = result.xmm1;
  }
  return result.rax;
}

static uint64_t test_simple0(void* sym) {
  uint64_t out = 0;
  return box64_call_simple(sym, 0, 0, 0, 0, 0, 0, &out) == BOX64_CALL_OK ? out : 0;
}
static uint64_t test_simple1(void* sym, uint64_t a0) {
  uint64_t out = 0;
  return box64_call_simple(sym, a0, 0, 0, 0, 0, 0, &out) == BOX64_CALL_OK ? out : 0;
}
static uint64_t test_simple2(void* sym, uint64_t a0, uint64_t a1) {
  uint64_t out = 0;
  return box64_call_simple(sym, a0, a1, 0, 0, 0, 0, &out) == BOX64_CALL_OK ? out : 0;
}
static uint64_t test_simple3(void* sym, uint64_t a0, uint64_t a1, uint64_t a2) {
  uint64_t out = 0;
  return box64_call_simple(sym, a0, a1, a2, 0, 0, 0, &out) == BOX64_CALL_OK ? out : 0;
}
static uint64_t test_simple4(void* sym, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
  uint64_t out = 0;
  return box64_call_simple(sym, a0, a1, a2, a3, 0, 0, &out) == BOX64_CALL_OK ? out : 0;
}
static uint64_t test_simple5(void* sym, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
  uint64_t out = 0;
  return box64_call_simple(sym, a0, a1, a2, a3, a4, 0, &out) == BOX64_CALL_OK ? out : 0;
}
static uint64_t test_simple6(void* sym, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
  uint64_t out = 0;
  return box64_call_simple(sym, a0, a1, a2, a3, a4, a5, &out) == BOX64_CALL_OK ? out : 0;
}

#endif
