/*
 * Regression for the compiled-layout seam test_desc_plan()
 * (tests/call_compat.h, built on box64_call_desc()) — the entry BUILD-TIME
 * generated stubs use.
 *
 * Three kinds of evidence, all against a real x86_64 guest object:
 *
 *   1. REGISTER PROBE (tests/guest/probe_guest.S): the guest stores the
 *      incoming rdi/rsi/rdx/rcx/r8/r9 and the full 16 bytes of xmm0/xmm1, so a
 *      sparse gpr_mask / fpr_mask can be checked byte for byte, including the
 *      high half of an SSE register that the old register-file APIs dropped.
 *
 *   2. COMPILER ORACLE (tests/guest/struct_guest.c): every `target_*` struct
 *      signature has an `oracle_*` wrapper the x86_64 compiler generated that
 *      takes the fields separately and calls the target. Running both through
 *      the seam and requiring equal results makes the compiler the authority
 *      on the aggregate classification. This is what catches a wrong reading
 *      such as "struct { float a, b, c; } goes in xmm0's low 12 bytes" — it is
 *      really two SSE eightbytes, xmm0 = {a,b} and xmm1 = {c}.
 *
 *   3. STACK placement, including lengths that are not 16-aligned (24 and 40
 *      byte structs), matched against the compiler's register-passed oracle.
 *
 * Link: whole-archive libbox64.a + libdynarec.a + the Rust facade.
 */
#include <dlfcn.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "libbox64.h"
#include "call_compat.h"

#include "os.h"
#include "regs.h"
#include "x64emu.h"
#include "emu/x64emu_private.h"

static int g_fail;
static int g_checks;

static void check(int cond, const char* what) {
  g_checks++;
  if (!cond) {
    g_fail = 1;
    printf("[call-plan] FAIL %s\n", what);
  } else {
    printf("[call-plan] ok   %s\n", what);
  }
}

static void check_u64(uint64_t got, uint64_t want, const char* what) {
  if (got != want)
    printf("[call-plan]      got=%#llx want=%#llx\n",
           (unsigned long long)got, (unsigned long long)want);
  check(got == want, what);
}

static void check_d(double got, double want, const char* what) {
  double diff = got - want;
  if (diff < 0) diff = -diff;
  if (!(diff <= 1e-9 * (1.0 + (want < 0 ? -want : want))))
    printf("[call-plan]      got=%.17g want=%.17g\n", got, want);
  check(diff <= 1e-9 * (1.0 + (want < 0 ? -want : want)), what);
}

static uint32_t fbits(float x) {
  uint32_t u;
  memcpy(&u, &x, 4);
  return u;
}
static uint64_t dbits(double x) {
  uint64_t u;
  memcpy(&u, &x, 8);
  return u;
}
static uint64_t pack2f(float x, float y) {
  return (uint64_t)fbits(x) | ((uint64_t)fbits(y) << 32);
}
static float lo_float(const box64_call_results_t* r) {
  float f;
  memcpy(&f, &r->xmm0.lo, 4);
  return f;
}
static double lo_double(const box64_call_results_t* r) {
  double d;
  memcpy(&d, &r->xmm0.lo, 8);
  return d;
}

static void* g_lib;
static void* sym(const char* name) {
  void* p = box64_dlsym(g_lib, name);
  if (!p)
    printf("[call-plan] dlsym %s: %s\n", name,
           box64_dlerror() ? box64_dlerror() : "?");
  return p;
}

/* Run a plan and return xmm0's low half as a double. */
static double plan_xmm0(void* s, const box64_sigplan_t* p, const uint64_t* gprs,
                        const box64_fpr_t* fprs, const void* stack) {
  box64_call_results_t r;
  memset(&r, 0, sizeof(r));
  test_desc_plan(s, p, gprs, fprs, stack, &r);
  return lo_double(&r);
}

/* Same, for a float return: only the low 32 bits of xmm0 are the value, the
 * upper lanes are not part of the return. */
static float plan_xmm0_f(void* s, const box64_sigplan_t* p,
                         const uint64_t* gprs, const box64_fpr_t* fprs,
                         const void* stack) {
  box64_call_results_t r;
  memset(&r, 0, sizeof(r));
  test_desc_plan(s, p, gprs, fprs, stack, &r);
  return lo_float(&r);
}

/* ---- callback reentrancy alignment -------------------------------------- */

static void* g_align_sym;
static uint64_t g_reentry_align;

static uint64_t cb_align6(uint64_t a, uint64_t b, uint64_t c, uint64_t d,
                          uint64_t e, uint64_t f) {
  (void)b;
  (void)c;
  (void)d;
  (void)e;
  (void)f;
  /* Re-enter the engine from inside a bridge callback and record the entry
   * parity the nested call sees. */
  g_reentry_align = test_simple0(g_align_sym);
  return a;
}

int main(int argc, char** argv) {
  void* s_probe, *s_s3f, *s_o_s3f, *s_s2d, *s_o_s2d, *s_sfd, *s_o_sfd;
  void* s_inner, *s_o_inner, *s_big, *s_o_big, *s_big5, *s_o_big5;
  void* s_align, *s_align_sse, *s_invoke;
  uint64_t gprs[6];
  box64_fpr_t fprs[8];
  uint8_t probe[80];
  int i;

  if (argc < 2) {
    printf("[call-plan] usage: %s <guest.so>\n", argv[0]);
    return 2;
  }
  box64_diag_set(0);
  if (box64_lib_init() != 0 || box64_ensure_emu() != 0) {
    printf("[call-plan] engine init: %s\n",
           box64_dlerror() ? box64_dlerror() : "?");
    return 1;
  }
  g_lib = box64_dlopen(argv[1], RTLD_NOW);
  if (!g_lib) {
    printf("[call-plan] dlopen %s: %s\n", argv[1],
           box64_dlerror() ? box64_dlerror() : "?");
    return 1;
  }

  s_probe = sym("probe_guest");
  s_s3f = sym("target_s3f");
  s_o_s3f = sym("oracle_s3f");
  s_s2d = sym("target_s2d");
  s_o_s2d = sym("oracle_s2d");
  s_sfd = sym("target_sfd");
  s_o_sfd = sym("oracle_sfd");
  s_inner = sym("target_i_fffp_p");
  s_o_inner = sym("oracle_i_fffp_p");
  s_big = sym("target_big");
  s_o_big = sym("oracle_big");
  s_big5 = sym("target_big5");
  s_o_big5 = sym("oracle_big5");
  s_align = sym("align_probe");
  s_align_sse = sym("align_probe_sse");
  s_invoke = sym("g_invoke");
  if (!s_probe || !s_s3f || !s_o_s3f || !s_s2d || !s_o_s2d || !s_sfd ||
      !s_o_sfd || !s_inner || !s_o_inner || !s_big || !s_o_big || !s_big5 ||
      !s_o_big5 || !s_align || !s_align_sse || !s_invoke) {
    printf("[call-plan] RESULT: FAIL (missing guest symbols)\n");
    return 1;
  }
  g_align_sym = s_align;

  /* --- 1. register probe: full sparse file, 16-byte SSE ---------------- */
  {
    static const box64_sigplan_t p = BOX64_SIGPLAN(0x3F, 0x03, 0x00, 0);
    memset(probe, 0, sizeof(probe));
    for (i = 0; i < 6; ++i) gprs[i] = 0xAAAA000000000000ull + (uint64_t)i;
    gprs[0] = (uint64_t)(uintptr_t)probe; /* probe writes its own rdi too */
    memset(fprs, 0, sizeof(fprs));
    fprs[0].lo = 0x1122334455667788ull;
    fprs[0].hi = 0x99aabbccddeeff00ull;
    fprs[1].lo = 0x0f1e2d3c4b5a6978ull;
    fprs[1].hi = 0x8877665544332211ull;
    test_desc_plan(s_probe, &p, gprs, fprs, NULL, NULL);

    check(memcmp(probe + 0, &gprs[0], 8) == 0, "probe: rdi written");
    check(memcmp(probe + 8, &gprs[1], 8) == 0, "probe: rsi written");
    check(memcmp(probe + 16, &gprs[2], 8) == 0, "probe: rdx written");
    check(memcmp(probe + 24, &gprs[3], 8) == 0, "probe: rcx written");
    check(memcmp(probe + 32, &gprs[4], 8) == 0, "probe: r8 written");
    check(memcmp(probe + 40, &gprs[5], 8) == 0, "probe: r9 written");
    check(memcmp(probe + 48, &fprs[0], 16) == 0,
          "probe: xmm0 full 16 bytes written");
    check(memcmp(probe + 64, &fprs[1], 16) == 0,
          "probe: xmm1 full 16 bytes written");
  }

  /* Sparse masks: only named positions need values. */
  {
    static const box64_sigplan_t p = BOX64_SIGPLAN(0x21, 0x02, 0x00, 0);
    memset(probe, 0, sizeof(probe));
    for (i = 0; i < 6; ++i) gprs[i] = 0xBBBB000000000000ull + (uint64_t)i;
    gprs[0] = (uint64_t)(uintptr_t)probe;
    memset(fprs, 0, sizeof(fprs));
    fprs[1].lo = 0x1234567890abcdefull;
    fprs[1].hi = 0xfedcba0987654321ull;
    test_desc_plan(s_probe, &p, gprs, fprs, NULL, NULL);
    check(memcmp(probe + 0, &gprs[0], 8) == 0, "sparse: rdi (bit 0) written");
    check(memcmp(probe + 40, &gprs[5], 8) == 0, "sparse: r9 (bit 5) written");
    check(memcmp(probe + 64, &fprs[1], 16) == 0,
          "sparse: xmm1 (bit 1) written in full");
  }

  /* --- 1b. ABI entry alignment (naked asm) -----------------------------
   * The SysV/Win64 entry contract is rsp % 16 == 8, because DynaCall's
   * PushExit(-8) pushes the exit slot and the first stack argument sits at
   * entry rsp+8 (16-aligned). The engine must produce the same parity on every
   * path — no stack, floating point, stack aggregates, and the descriptor adapter coverage — or a
   * callee's aligned SSE access faults. align_probe() reports rsp%16 directly;
   * align_probe_sse() does a real movaps to [rsp+8] (a hard fault if wrong). */
  {
    static const box64_sigplan_t p_none = BOX64_SIGPLAN(0x00, 0x00, 0x01, 0);
    static const box64_sigplan_t p_fp = BOX64_SIGPLAN(0x00, 0x01, 0x00, 0);
    static const box64_sigplan_t p_st24 = BOX64_SIGPLAN(0x00, 0x00, 0x01, 24);
    static const box64_sigplan_t p_st40 = BOX64_SIGPLAN(0x00, 0x00, 0x01, 40);
    uint64_t ag[6] = {0};
    uint64_t af[8] = {0};
    box64_fpr_t pf[8];
    uint8_t blob[40];
    memset(pf, 0, sizeof(pf));
    memset(blob, 0, sizeof(blob));

    check_u64(test_simple0(s_align), 8, "align: call0 entry rsp%16 == 8");
    check_u64(test_simple1(s_align, 0), 8, "align: call1 entry rsp%16 == 8");
    check_u64(test_desc_plan(s_align, &p_none, ag, NULL, NULL, NULL), 8,
              "align: plan(no stack) entry rsp%16 == 8");
    check_u64(test_desc_plan(s_align, &p_st24, ag, NULL, blob, NULL), 8,
              "align: plan(24B stack) entry rsp%16 == 8");
    check_u64(test_desc_plan(s_align, &p_st40, ag, NULL, blob, NULL), 8,
              "align: plan(40B stack) entry rsp%16 == 8");
    check_u64(test_desc_regs(s_align, ag, af), 8,
              "align: descriptor register-file adapter entry rsp%16 == 8");
    check_u64(test_desc_blob(s_align, ag, af, blob, 24), 8,
              "align: descriptor blob adapter(24B) entry rsp%16 == 8");
    check_u64(test_desc_blob(s_align, ag, af, NULL, 0), 8,
              "align: descriptor blob adapter(no stack) entry rsp%16 == 8");

    /* Real aligned SSE access. If the parity is wrong this SIGSEGVs, which is
     * the point: the check is not a value comparison. */
    test_desc_plan(s_align_sse, &p_fp, NULL, pf, NULL, NULL);
    check(1, "align: movaps to [rsp+8] succeeded (16-byte aligned slot)");
  }

  /* --- 2. compiler oracle: s3f, two SSE eightbytes --------------------- */
  {
    static const box64_sigplan_t p_t = BOX64_SIGPLAN(0x00, 0x03, 0x04, 0);
    static const box64_sigplan_t p_o = BOX64_SIGPLAN(0x00, 0x07, 0x04, 0);
    float a = 1.5f, b = -2.25f, c = 3.125f;
    box64_fpr_t tf[8], of[8];
    memset(tf, 0, sizeof(tf));
    memset(of, 0, sizeof(of));
    tf[0].lo = pack2f(a, b);
    tf[1].lo = fbits(c);
    of[0].lo = fbits(a);
    of[1].lo = fbits(b);
    of[2].lo = fbits(c);
    float got = plan_xmm0_f(s_s3f, &p_t, NULL, tf, NULL);
    float want = plan_xmm0_f(s_o_s3f, &p_o, NULL, of, NULL);
    check_d((double)got, (double)want,
            "compiler oracle: struct{float,float,float} = xmm0{a,b}, xmm1{c}");
  }

  /* --- s2d: two SSE eightbytes, one register each (SSEUP is only for vector
   * types, so b is in xmm1, NOT xmm0's high half). A regression to the
   * "xmm0.lo/xmm0.hi" reading fails here: target_s2d would see b == 0. ----- */
  {
    static const box64_sigplan_t p_t = BOX64_SIGPLAN(0x00, 0x03, 0x04, 0);
    static const box64_sigplan_t p_o = BOX64_SIGPLAN(0x00, 0x03, 0x04, 0);
    double a = 1.5, b = -2.25;
    box64_fpr_t tf[8], of[8];
    memset(tf, 0, sizeof(tf));
    memset(of, 0, sizeof(of));
    tf[0].lo = dbits(a);
    tf[1].lo = dbits(b);
    of[0].lo = dbits(a);
    of[1].lo = dbits(b);
    check_d(plan_xmm0(s_s2d, &p_t, NULL, tf, NULL),
            plan_xmm0(s_o_s2d, &p_o, NULL, of, NULL),
            "compiler oracle: struct{double,double} = xmm0{a}, xmm1{b}");
  }

  /* --- sfd: float in one SSE eightbyte, double in the next ------------- */
  {
    static const box64_sigplan_t p = BOX64_SIGPLAN(0x00, 0x03, 0x04, 0);
    float a = 4.5f;
    double b = -6.25;
    box64_fpr_t tf[8], of[8];
    memset(tf, 0, sizeof(tf));
    memset(of, 0, sizeof(of));
    tf[0].lo = fbits(a);
    tf[1].lo = dbits(b);
    of[0].lo = fbits(a);
    of[1].lo = dbits(b);
    check_d(plan_xmm0(s_sfd, &p, NULL, tf, NULL),
            plan_xmm0(s_o_sfd, &p, NULL, of, NULL),
            "compiler oracle: struct{float,double} = xmm0, xmm1");
  }

  /* --- i(fffp)p: the 24-byte struct exceeds two eightbytes, so the compiler
   * classifies it MEMORY and passes it on the stack; the separate pointer is
   * the next INTEGER argument (rsi). The oracle, oracle_i_fffp_p(long, float,
   * float, float, void*, void*), takes the fields separately. --------------- */
  {
    /* target: rdi=a0, rsi=p; 24-byte struct on the stack {a,b,c,pad,sp} */
    static const box64_sigplan_t p_t = BOX64_SIGPLAN(0x03, 0x00, 0x04, 24);
    /* oracle: rdi=a0, rsi=sp, rdx=p; xmm0=a, xmm1=b, xmm2=c */
    static const box64_sigplan_t p_o = BOX64_SIGPLAN(0x07, 0x07, 0x04, 0);
    float a = 1.25f, b = 2.5f, c = 4.75f;
    void* sp = (void*)0x1234;
    void* p = (void*)0x5678;
    uint64_t tg[6] = {0}, og[6] = {0};
    box64_fpr_t of[8];
    uint8_t blob[24];
    uint32_t fa = fbits(a), fb = fbits(b), fc = fbits(c);
    uint64_t spv = (uint64_t)(uintptr_t)sp;
    box64_call_results_t rt, ro;
    tg[0] = 7;
    tg[1] = (uint64_t)(uintptr_t)p;
    og[0] = 7;
    og[1] = (uint64_t)(uintptr_t)sp;
    og[2] = (uint64_t)(uintptr_t)p;
    memset(of, 0, sizeof(of));
    of[0].lo = fbits(a);
    of[1].lo = fbits(b);
    of[2].lo = fbits(c);
    memset(blob, 0, sizeof(blob));
    memcpy(blob + 0, &fa, 4);
    memcpy(blob + 4, &fb, 4);
    memcpy(blob + 8, &fc, 4);
    memcpy(blob + 16, &spv, 8);
    memset(&rt, 0, sizeof(rt));
    memset(&ro, 0, sizeof(ro));
    test_desc_plan(s_inner, &p_t, tg, NULL, blob, &rt);
    test_desc_plan(s_o_inner, &p_o, og, of, NULL, &ro);
    check_d(lo_double(&rt), lo_double(&ro),
            "compiler oracle: i(fffp)p 24B struct is MEMORY (stack), p in rsi");
  }

  /* --- 3. stack aggregates, unaligned byte counts ---------------------- */
  {
    /* 24 bytes: rdi=x, stack = {a,b,c}; oracle passes them in registers. */
    static const box64_sigplan_t p_t = BOX64_SIGPLAN(0x01, 0x00, 0x01, 24);
    static const box64_sigplan_t p_o = BOX64_SIGPLAN(0x0F, 0x00, 0x01, 0);
    long blob[3] = {11, 22, 33};
    uint64_t tg[6] = {5}, og[6] = {5, 11, 22, 33};
    box64_call_results_t rt, ro;
    memset(&rt, 0, sizeof(rt));
    memset(&ro, 0, sizeof(ro));
    test_desc_plan(s_big, &p_t, tg, NULL, blob, &rt);
    test_desc_plan(s_o_big, &p_o, og, NULL, NULL, &ro);
    check_u64(rt.rax, ro.rax, "compiler oracle: 24-byte struct on the stack");
  }
  {
    /* 40 bytes (not 16-aligned): engine must round up to 48. */
    static const box64_sigplan_t p_t = BOX64_SIGPLAN(0x01, 0x00, 0x01, 40);
    static const box64_sigplan_t p_o = BOX64_SIGPLAN(0x3F, 0x00, 0x01, 0);
    long blob[5] = {1, 2, 3, 4, 5};
    uint64_t tg[6] = {100}, og[6] = {100, 1, 2, 3, 4, 5};
    box64_call_results_t rt, ro;
    memset(&rt, 0, sizeof(rt));
    memset(&ro, 0, sizeof(ro));
    test_desc_plan(s_big5, &p_t, tg, NULL, blob, &rt);
    test_desc_plan(s_o_big5, &p_o, og, NULL, NULL, &ro);
    check_u64(rt.rax, ro.rax,
              "compiler oracle: 40-byte struct on the stack (engine aligns)");
  }

  /* --- reentrancy: alignment holds for a nested call ------------------- */
  {
    void* cb = box64_callback((void*)cb_align6);
    check(cb != NULL, "reentry: box64_callback returns an address");
    if (cb) {
      check_u64(test_simple2(s_invoke, (uint64_t)(uintptr_t)cb, 5), 6,
                "reentry: g_invoke(cb, 5) == 6");
      check_u64(g_reentry_align, 8,
                "align: re-entrant call entry rsp%16 == 8");
    }
  }

  /* --- layout validation ----------------------------------------------- */
  {
    static const box64_sigplan_t bad_gpr = BOX64_SIGPLAN(0x40, 0, 0, 0);
    static const box64_sigplan_t bad_fpr = BOX64_SIGPLAN(0, 0x100, 0, 0);
    static const box64_sigplan_t bad_ret = BOX64_SIGPLAN(0, 0, 0x10, 0);
    static const box64_sigplan_t need_arrays = BOX64_SIGPLAN(0x01, 0x01, 0, 0);
    static const box64_sigplan_t need_stack = BOX64_SIGPLAN(0, 0, 0, 16);
    uint64_t g[6] = {0};
    box64_fpr_t f[8];
    memset(f, 0, sizeof(f));
    check_u64(test_desc_plan(s_probe, NULL, g, f, NULL, NULL), 0,
              "plan: NULL plan refused");
    check(box64_dlerror() != NULL, "plan: NULL plan records an error");
    check_u64(test_desc_plan(s_probe, &bad_gpr, g, f, NULL, NULL), 0,
              "plan: gpr_mask outside 0..5 refused");
    check_u64(test_desc_plan(s_probe, &bad_fpr, g, f, NULL, NULL), 0,
              "plan: fpr_mask outside 0..7 refused");
    check_u64(test_desc_plan(s_probe, &bad_ret, g, f, NULL, NULL), 0,
              "plan: ret_mask outside 0..3 refused");
    check_u64(test_desc_plan(s_probe, &need_arrays, NULL, NULL, NULL, NULL), 0,
              "plan: mask with NULL value arrays refused");
    check_u64(test_desc_plan(s_probe, &need_stack, g, f, NULL, NULL), 0,
              "plan: stack_len with NULL stack refused");
  }

  if (g_fail) {
    printf("[call-plan] RESULT: FAIL (%d checks)\n", g_checks);
    return 1;
  }
  printf("[call-plan] RESULT: PASS (%d checks)\n", g_checks);
  return 0;
}
