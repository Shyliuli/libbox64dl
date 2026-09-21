/*
 * Regression for the fixed-arity call entries (box64_call0..6) in
 * include/libbox64.h, the diagnostics switch, and the unchanged behaviour of
 * the old register-file APIs.
 *
 * The callee is a real x86_64 shared object (tests/guest/call_guest.c) loaded
 * through box64_dlopen/dlsym, and every assertion compares against a value the
 * guest itself computes. A fast path that dropped an argument register, mixed
 * up the SSE/stack groups or corrupted the emu would fail here.
 *
 * Covered:
 *   1. 0 / 1 / 2 / 6 integer arguments; high-bit, negative 32- and 64-bit
 *      values; a pointer argument; a constructor marker;
 *   2. the 0-return / error-slot contract: a guest returning 0 is NOT a
 *      refusal (box64_dlerror() stays clear), a NULL symbol is;
 *   3. the old APIs still agree: box64_call_regs and box64_call_marshaled with
 *      a real outgoing stack blob;
 *   4. stack preservation: the engine restores the pre-call SP, and the guest
 *      can recurse;
 *   5. callback reentrancy: the guest calls a host bridge which re-enters the
 *      engine through box64_call1;
 *   6. multithreading: several host threads call the guest concurrently, each
 *      on its own emu;
 *   7. diagnostics: default follows BOX64DL_DIAG (off when unset), and
 *      box64_diag_set() overrides;
 *   8. known-ABI preservation: the fixed-arity entries and box64_call_plan go
 *      through DynaCall_abi, which drops only the dead RBX/RBP/RDI/RSI
 *      re-restore. A legal guest keeps its non-volatile set (RBX/RBP/R12-R15)
 *      because it restored them itself, and leaves the SysV volatile set
 *      (RDI/RSI/RCX/R8-R11) with the callee; box64_call_regs/marshaled keep the
 *      old generic DynaCall behavior (RDI/RSI restored), and a nested ABI call
 *      from a host bridge keeps the outer call's non-volatile registers;
 *   9. the longjmp branch: both entries keep the engine's no-register-restore
 *      rule, driven as a unit through emu->flags (a real guest longjmp, and
 *      signal/fault paths, are NOT covered by this suite).
 *
 * Link: whole-archive libbox64.a + libdynarec.a + the Rust facade, exactly
 * like a host embedder.
 */
#include <dlfcn.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libbox64.h"

#include "os.h"
#include "regs.h"
#include "x64emu.h"
#include "emu/x64emu_private.h"

/* threads.c (patch 0003): this thread's own emu, never fabricated. */
extern x64emu_t* thread_peek_emu(void);
/* rust/src/lib.rs: sets the thread-local box64_dlerror() slot (used here to
 * plant a stale error before a call). */
extern void box64_set_err(const char* msg);

static int g_fail;
static int g_checks;

static void check(int cond, const char* what) {
  g_checks++;
  if (!cond) {
    g_fail = 1;
    printf("[call-arity] FAIL %s\n", what);
  } else {
    printf("[call-arity] ok   %s\n", what);
  }
}

static void check_u64(uint64_t got, uint64_t want, const char* what) {
  if (got != want) {
    printf("[call-arity]      got=%#llx want=%#llx\n",
           (unsigned long long)got, (unsigned long long)want);
  }
  check(got == want, what);
}

static void* g_lib;

static void* sym(const char* name) {
  void* p = box64_dlsym(g_lib, name);
  if (!p)
    printf("[call-arity] dlsym %s failed: %s\n", name,
           box64_dlerror() ? box64_dlerror() : "?");
  return p;
}

/* ---- callback reentrancy ------------------------------------------------ */

static void* g_id_sym;

/* The engine's default bridge forwards six integer registers, so take six and
 * use the first; from inside it, re-enter the engine. */
static uint64_t host_cb6(uint64_t a, uint64_t b, uint64_t c, uint64_t d,
                         uint64_t e, uint64_t f) {
  (void)b;
  (void)c;
  (void)d;
  (void)e;
  (void)f;
  return box64_call1(g_id_sym, a * 3);
}

/* ---- multithreading ----------------------------------------------------- */

struct mt_arg {
  void* add2;
  uint64_t idx;
  int fail;
  uint64_t emu;
};

static void* mt_thread(void* p) {
  struct mt_arg* a = (struct mt_arg*)p;
  int i;

  if (box64_ensure_emu() != 0) {
    a->fail = 1;
    return NULL;
  }
  a->emu = (uint64_t)(uintptr_t)thread_peek_emu();
  if (!a->emu) {
    a->fail = 1;
    return NULL;
  }
  for (i = 0; i < 500; ++i) {
    uint64_t got = box64_call2(a->add2, a->idx, (uint64_t)i);
    if (got != a->idx + (uint64_t)i) {
      a->fail = 1;
      break;
    }
  }
  return NULL;
}

int main(int argc, char** argv) {
  x64emu_t* emu;
  void* s_zero, *s_retzero, *s_ctor, *s_id, *s_add2, *s_sum6, *s_sum10;
  void* s_neg32, *s_neg64, *s_ptr, *s_invoke, *s_deep;
  uint64_t gregs[6], fregs[8], mg[6], mf[8], blob[4];
  uint64_t rsp_before, rsp_after;
  long val;
  uint64_t r;
  int i;

  if (argc < 2) {
    printf("[call-arity] usage: %s <guest.so>\n", argv[0]);
    return 2;
  }

  /* 7. diagnostics: the first get() performs the lazy BOX64DL_DIAG read. */
  {
    const char* env = getenv("BOX64DL_DIAG");
    int expect = (env && env[0] && strcmp(env, "0") != 0) ? 1 : 0;
    check(box64_diag_get() == expect,
          "diagnostics level follows BOX64DL_DIAG (default 0/off)");
    box64_diag_set(1);
    check(box64_diag_get() == 1, "box64_diag_set(1) enables");
    box64_diag_set(0);
    check(box64_diag_get() == 0, "box64_diag_set(0) silences");
  }

  if (box64_lib_init() != 0) {
    printf("[call-arity] box64_lib_init: %s\n",
           box64_dlerror() ? box64_dlerror() : "?");
    return 1;
  }
  if (box64_ensure_emu() != 0) {
    printf("[call-arity] box64_ensure_emu: %s\n",
           box64_dlerror() ? box64_dlerror() : "?");
    return 1;
  }
  emu = thread_peek_emu();
  if (!emu) {
    printf("[call-arity] no emu on the main thread\n");
    return 1;
  }

  g_lib = box64_dlopen(argv[1], RTLD_NOW);
  if (!g_lib) {
    printf("[call-arity] dlopen %s: %s\n", argv[1],
           box64_dlerror() ? box64_dlerror() : "?");
    return 1;
  }

  s_zero = sym("g_zero");
  s_retzero = sym("g_retzero");
  s_ctor = sym("g_ctor_marker");
  s_id = sym("g_id");
  s_add2 = sym("g_add2");
  s_sum6 = sym("g_sum6");
  s_sum10 = sym("g_sum10");
  s_neg32 = sym("g_neg32");
  s_neg64 = sym("g_neg64");
  s_ptr = sym("g_ptr_long");
  s_invoke = sym("g_invoke");
  s_deep = sym("g_deep");
  if (!s_zero || !s_retzero || !s_ctor || !s_id || !s_add2 || !s_sum6 ||
      !s_sum10 || !s_neg32 || !s_neg64 || !s_ptr || !s_invoke || !s_deep) {
    printf("[call-arity] RESULT: FAIL (missing guest symbols)\n");
    return 1;
  }
  g_id_sym = s_id;

  /* --- 1/2. fixed arity, 0/error contract ------------------------------ */

  check_u64(box64_call0(s_zero), 0x1234, "call0: 0 args, non-zero return");
  check_u64(box64_call0(s_ctor), 0x5a5a5a5a5a5a5a5aULL,
            "call0: guest DT_INIT_ARRAY constructor ran");
  check_u64(box64_call0(s_retzero), 0, "call0: legitimate 0 return");
  check(box64_dlerror() == NULL,
        "call0: a guest 0 is not a refusal (error slot clear)");

  check_u64(box64_call1(s_id, 0xdeadbeefcafef00dULL),
            0xdeadbeefcafef00dULL, "call1: high-bit identity");
  check_u64(box64_call1(s_id, (uint64_t)-1), (uint64_t)-1,
            "call1: pointer-width negative (-1)");
  check_u64(box64_call2(s_add2, 20, 22), 42, "call2: 20 + 22");
  check_u64(box64_call2(s_add2, (uint64_t)-3, 3), 0,
            "call2: negative + positive");
  check_u64(box64_call6(s_sum6, 1, 2, 3, 4, 5, 6), 21, "call6: 1..6 = 21");
  check_u64(box64_call6(s_sum6, 1, 2, 3, 4, 5, (uint64_t)-6), 9,
            "call6: signed value in the last argument register");

  /* --- negative int sign handling -------------------------------------- */
  check_u64(box64_call1(s_neg32, (uint64_t)(int32_t)-5), 5,
            "neg32: -(-5) == 5 (32-bit argument sign-extended correctly)");
  check_u64(box64_call1(s_neg32, 5), (uint64_t)(int64_t)-5,
            "neg32: -(5) == -5 sign-extended in RAX");
  check_u64(box64_call1(s_neg64, 5), (uint64_t)(int64_t)-5,
            "neg64: -(5) == -5");
  check((int64_t)box64_call1(s_neg64, (uint64_t)(int64_t)-7) == 7,
        "neg64: -(-7) == 7");

  /* --- pointer argument ------------------------------------------------ */
  val = 0x1122334455667788L;
  check_u64(box64_call1(s_ptr, (uint64_t)(uintptr_t)&val), (uint64_t)val,
            "call1: pointer argument dereferenced in the guest");

  /* --- 4. stack preservation ------------------------------------------- */
  rsp_before = emu->regs[_SP].q[0];
  check_u64(box64_call1(s_deep, 400), 400, "call1: guest recursion used its stack");
  rsp_after = emu->regs[_SP].q[0];
  check(rsp_after == rsp_before,
        "callN: engine restored the pre-call SP exactly");

  /* --- 3. old register-file APIs still agree --------------------------- */
  for (i = 0; i < 6; ++i) gregs[i] = (uint64_t)(i + 1);
  memset(fregs, 0, sizeof(fregs));
  r = box64_call_regs(s_sum6, gregs, fregs);
  check_u64(r, 21, "old call_regs: sum6 argument file");
  check_u64(gregs[0], 21, "old call_regs: gregs[0] carries the return");

  memset(gregs, 0, sizeof(gregs));
  check_u64(box64_call_regs(s_zero, gregs, fregs), 0x1234,
            "old call_regs: 0-argument call still works");

  for (i = 0; i < 6; ++i) mg[i] = (uint64_t)(i + 1);
  for (i = 0; i < 4; ++i) blob[i] = (uint64_t)(i + 7);
  memset(mf, 0, sizeof(mf));
  rsp_before = emu->regs[_SP].q[0];
  r = box64_call_marshaled(s_sum10, mg, mf, blob, sizeof(blob));
  rsp_after = emu->regs[_SP].q[0];
  check_u64(r, 55, "old call_marshaled: 6 register + 4 stack args = 55");
  check(rsp_after == rsp_before,
        "old call_marshaled: outgoing stack fully reclaimed");

  /* --- engine refusal is distinguishable ------------------------------- */
  check_u64(box64_call0(NULL), 0, "call0(NULL): refusal returns 0");
  check(box64_dlerror() != NULL, "call0(NULL): refusal records an error");
  check(box64_dlerror() == NULL, "error slot is read-and-consume");

  /* --- a successful call CLEARS a stale error slot ---------------------- */
  /* Plant a stale error and do NOT read it first: the successful call must
   * clear it, so the slot is empty afterwards. Reading first would consume it
   * and prove nothing about the clear. */
  box64_set_err("stale");
  check_u64(box64_call0(s_zero), 0x1234, "stale: a real call still runs");
  check(box64_dlerror() == NULL, "stale: a successful call cleared the slot");

  /* A refusal must overwrite a stale error, not hand the stale text back. */
  box64_set_err("stale2");
  check_u64(box64_call0(NULL), 0, "stale2: refusal returns 0");
  {
    const char* e = box64_dlerror();
    check(e != NULL, "stale2: refusal records a fresh error");
    check(e != NULL && strcmp(e, "stale2") != 0,
          "stale2: refusal overwrote the stale text");
  }

  /* --- 5. callback reentrancy ------------------------------------------ */
  {
    void* cb = box64_callback((void*)host_cb6, "ll");
    check(cb != NULL, "box64_callback returns a guest-callable address");
    if (cb) {
      check_u64(box64_call2(s_invoke, (uint64_t)(uintptr_t)cb, 7), 22,
                "callback re-enters the engine (7*3+1 == 22)");
    }
  }

  /* --- 8. known-ABI preservation contract ------------------------------ */
  {
    const uint64_t NV_BX = 0xb0b0b0b0b0b0b0b0ULL;
    const uint64_t NV_BP = 0xb1b1b1b1b1b1b1b1ULL;
    const uint64_t NV_R12 = 0xc2c2c2c2c2c2c2c2ULL;
    const uint64_t NV_R13 = 0xd3d3d3d3d3d3d3d3ULL;
    const uint64_t NV_R14 = 0xe4e4e4e4e4e4e4e4ULL;
    const uint64_t NV_R15 = 0xf5f5f5f5f5f5f5f5ULL;
    const uint64_t V_DI = 0x1111111111111111ULL;
    const uint64_t V_SI = 0x2222222222222222ULL;
    const uint64_t V_CX = 0x4444444444444444ULL;
    const uint64_t V_R8 = 0x5555555555555555ULL;
    const uint64_t V_R9 = 0x6666666666666666ULL;
    const uint64_t MARK = 0xabcdef0123456789ULL;
    void* s_regs = sym("regs_probe");
    void* s_around = sym("regs_around_callback");

    check(s_regs != NULL && s_around != NULL, "ABI probes are exported");

    /* ABI mode (box64_call0): the callee restored its non-volatile set, so it
     * survives; the SysV volatile set is the callee's to change and must NOT
     * be asserted as preserved. */
    emu->regs[_BX].q[0] = NV_BX;
    emu->regs[_BP].q[0] = NV_BP;
    emu->regs[_R12].q[0] = NV_R12;
    emu->regs[_R13].q[0] = NV_R13;
    emu->regs[_R14].q[0] = NV_R14;
    emu->regs[_R15].q[0] = NV_R15;
    check_u64(box64_call0(s_regs), MARK, "ABI mode: call0 reaches the guest");
    check_u64(emu->regs[_BX].q[0], NV_BX, "ABI mode: RBX preserved (non-volatile)");
    check_u64(emu->regs[_BP].q[0], NV_BP, "ABI mode: RBP preserved (non-volatile)");
    check_u64(emu->regs[_R12].q[0], NV_R12, "ABI mode: R12 preserved (non-volatile)");
    check_u64(emu->regs[_R13].q[0], NV_R13, "ABI mode: R13 preserved (non-volatile)");
    check_u64(emu->regs[_R14].q[0], NV_R14, "ABI mode: R14 preserved (non-volatile)");
    check_u64(emu->regs[_R15].q[0], NV_R15, "ABI mode: R15 preserved (non-volatile)");
    check_u64(emu->regs[_DI].q[0], V_DI, "ABI mode: RDI left to the callee (SysV volatile)");
    check_u64(emu->regs[_SI].q[0], V_SI, "ABI mode: RSI left to the callee (SysV volatile)");
    check_u64(emu->regs[_CX].q[0], V_CX, "ABI mode: RCX volatile clobber visible");
    check_u64(emu->regs[_R8].q[0], V_R8, "ABI mode: R8 volatile clobber visible");
    check_u64(emu->regs[_R9].q[0], V_R9, "ABI mode: R9 volatile clobber visible");

    /* The plan seam is known-ABI mode too. */
    {
      box64_sigplan_t plan = BOX64_SIGPLAN(0, 0, 1, 0);
      box64_call_results_t out;
      memset(&out, 0, sizeof(out));
      emu->regs[_BX].q[0] = NV_BX;
      emu->regs[_R12].q[0] = NV_R12;
      r = box64_call_plan(s_regs, &plan, NULL, NULL, NULL, &out);
      check_u64(r, MARK, "ABI mode: call_plan reaches the guest");
      check_u64(out.rax, MARK, "ABI mode: call_plan ret_mask rax");
      check_u64(emu->regs[_BX].q[0], NV_BX, "ABI mode: call_plan keeps RBX");
      check_u64(emu->regs[_R12].q[0], NV_R12, "ABI mode: call_plan keeps R12");
      check_u64(emu->regs[_DI].q[0], V_DI, "ABI mode: call_plan leaves RDI to the callee");
    }

    /* Generic mode keeps the historical DynaCall rule: RDI/RSI come back from
     * the caller's register file. box64_call_regs() writes the return value
     * into gregs[0]/[1], so keep the inputs before the call. */
    for (i = 0; i < 6; ++i) gregs[i] = 0x1000 + (uint64_t)i;
    memset(fregs, 0, sizeof(fregs));
    emu->regs[_BX].q[0] = NV_BX;
    emu->regs[_BP].q[0] = NV_BP;
    emu->regs[_R12].q[0] = NV_R12;
    emu->regs[_R13].q[0] = NV_R13;
    emu->regs[_R14].q[0] = NV_R14;
    emu->regs[_R15].q[0] = NV_R15;
    {
      const uint64_t di_in = gregs[0];
      const uint64_t si_in = gregs[1];
      r = box64_call_regs(s_regs, gregs, fregs);
      check_u64(r, MARK, "generic mode: call_regs reaches the guest");
      check_u64(emu->regs[_DI].q[0], di_in,
                "generic mode: DynaCall still restores RDI");
      check_u64(emu->regs[_SI].q[0], si_in,
                "generic mode: DynaCall still restores RSI");
    }
    check_u64(emu->regs[_BX].q[0], NV_BX, "generic mode: RBX preserved");
    check_u64(emu->regs[_BP].q[0], NV_BP, "generic mode: RBP preserved");
    check_u64(emu->regs[_R12].q[0], NV_R12, "generic mode: R12 preserved");
    check_u64(emu->regs[_CX].q[0], V_CX, "generic mode: RCX stays with the callee");
    check_u64(emu->regs[_R8].q[0], V_R8, "generic mode: R8 stays with the callee");

    /* A nested ABI call from a host bridge must leave the outer call's
     * non-volatile registers alone. */
    {
      void* cb = box64_callback((void*)host_cb6, "ll");
      check(cb != NULL, "ABI nesting: box64_callback returns an address");
      if (cb)
        check_u64(box64_call2(s_around, (uint64_t)(uintptr_t)cb, 7), 1,
                  "ABI mode: nested bridge call keeps the outer R12");
    }
  }

  /* --- 9. longjmp branch: both entries keep the no-restore rule -------- */
  {
    void* s_regs = sym("regs_probe");
    const uint64_t BOGUS_IP = 0xdead0000beef0000ULL;
    const uint64_t MARK = 0xabcdef0123456789ULL;
    const uint64_t ALIGNED_SP = emu->regs[_SP].q[0] & ~(uint64_t)0xF;
    uint64_t old_sp = emu->regs[_SP].q[0];
    uint64_t old_ip = emu->ip.q[0];
    int old_qol = (int)emu->flags.quitonlongjmp;
    int old_lj = (int)emu->flags.longjmp;

    if (!s_regs) {
      check(0, "longjmp unit: regs_probe is exported");
    } else {
      /* Make both "was it restored?" probes distinguishable from the guest's
       * final state: RIP starts at a value the exit sentinel can never be, and
       * RSP starts 8-mod-16 so call_align() leaves the no-restore result
       * (aligned) different from the restored one (the 8-mod-16 value). */
      emu->ip.q[0] = BOGUS_IP;
      emu->regs[_SP].q[0] = ALIGNED_SP | 0x8u;
      emu->flags.quitonlongjmp = 1;
      emu->flags.longjmp = 1;
      r = box64_call0(s_regs);
      check_u64(r, MARK, "longjmp branch: ABI entry returns the guest RAX");
      check(emu->flags.quitonlongjmp == 1, "longjmp branch: quitonlongjmp kept");
      check(emu->flags.longjmp == 0,
            "longjmp branch: one-shot longjmp flag cleared (==1 rule)");
      check(emu->ip.q[0] != BOGUS_IP,
            "longjmp branch: ABI entry did not restore RIP");
      check_u64(emu->regs[_SP].q[0], ALIGNED_SP,
                "longjmp branch: ABI entry did not restore RSP");
      emu->regs[_SP].q[0] = old_sp;
      emu->ip.q[0] = old_ip;

      for (i = 0; i < 6; ++i) gregs[i] = 0x2000 + (uint64_t)i;
      memset(fregs, 0, sizeof(fregs));
      emu->ip.q[0] = BOGUS_IP;
      emu->regs[_SP].q[0] = ALIGNED_SP | 0x8u;
      emu->flags.quitonlongjmp = 1;
      emu->flags.longjmp = 1;
      r = box64_call_regs(s_regs, gregs, fregs);
      check_u64(r, MARK, "longjmp branch: generic entry returns the guest RAX");
      check(emu->flags.longjmp == 0,
            "longjmp branch: generic entry applies the same rule");
      check(emu->ip.q[0] != BOGUS_IP,
            "longjmp branch: generic entry did not restore RIP");
      check_u64(emu->regs[_SP].q[0], ALIGNED_SP,
                "longjmp branch: generic entry did not restore RSP");
      emu->regs[_SP].q[0] = old_sp;
      emu->ip.q[0] = old_ip;
    }

    emu->flags.quitonlongjmp = old_qol;
    emu->flags.longjmp = old_lj;
  }

  /* --- 6. multithreading ----------------------------------------------- */
  {
    enum { NTH = 4 };
    pthread_t th[NTH];
    struct mt_arg ma[NTH];
    int spawned = 0;

    for (i = 0; i < NTH; ++i) {
      ma[i].add2 = s_add2;
      ma[i].idx = (uint64_t)(100 + i);
      ma[i].fail = 0;
      ma[i].emu = 0;
      if (pthread_create(&th[i], NULL, mt_thread, &ma[i]) != 0) {
        check(0, "spawn worker thread");
        break;
      }
      spawned++;
    }
    for (i = 0; i < spawned; ++i) pthread_join(th[i], NULL);
    for (i = 0; i < spawned; ++i) {
      char label[64];
      snprintf(label, sizeof(label), "thread %d: 500 calls on its own emu", i);
      check(!ma[i].fail, label);
      check(ma[i].emu != 0 && ma[i].emu != (uint64_t)(uintptr_t)emu,
            "thread emu exists and differs from the main emu");
      if (i > 0)
        check(ma[i].emu != ma[i - 1].emu, "each thread has a distinct emu");
    }
  }

  if (g_fail) {
    printf("[call-arity] RESULT: FAIL (%d checks)\n", g_checks);
    return 1;
  }
  printf("[call-arity] RESULT: PASS (%d checks)\n", g_checks);
  return 0;
}
