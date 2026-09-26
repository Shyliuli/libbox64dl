/* Coverage for the unified status/descriptor call layer. */
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "libbox64.h"

static int failures;
static int checks;

static void check(int ok, const char* what) {
  ++checks;
  if (!ok) {
    ++failures;
    printf("[call-unified] FAIL %s\n", what);
  } else {
    printf("[call-unified] ok   %s\n", what);
  }
}

static void check_status(box64_call_status_t got, box64_call_status_t want,
                         const char* what) {
  if (got != want)
    printf("[call-unified]      got=%d want=%d\n", (int)got, (int)want);
  check(got == want, what);
}

static box64_call_desc_t make_desc(uint32_t flags, uint16_t gpr_mask,
                                   uint16_t fpr_mask, uint16_t ret_mask,
                                   uint32_t stack_len, const uint64_t* gprs,
                                   const box64_fpr_t* fprs, const void* stack) {
  box64_call_desc_t d;
  memset(&d, 0, sizeof(d));
  d.flags = flags;
  d.gpr_mask = gpr_mask;
  d.fpr_mask = fpr_mask;
  d.ret_mask = ret_mask;
  d.stack_len = stack_len;
  d.gprs = BOX64_PTR_TO_U64(gprs);
  d.fprs = BOX64_PTR_TO_U64(fprs);
  d.stack = BOX64_PTR_TO_U64(stack);
  return d;
}

static void* lib;
static void* guest_sym(const char* name) {
  void* p = box64_dlsym(lib, name);
  if (!p) {
    const char* e = box64_dlerror();
    printf("[call-unified] dlsym %s failed: %s\n", name, e ? e : "?");
  }
  return p;
}

static void* id_sym;
static uint64_t reenter_host(uint64_t a, uint64_t b, uint64_t c,
                             uint64_t d, uint64_t e, uint64_t f) {
  uint64_t result = 0;
  (void)b;
  (void)c;
  (void)d;
  (void)e;
  (void)f;
  if (box64_call_simple(id_sym, a * 3, 0, 0, 0, 0, 0, &result) !=
      BOX64_CALL_OK)
    return 0;
  return result;
}

/* Distinct wrapper identities deliberately share one host function.  The
 * callback bridge map must not return the first thunk for both signatures. */
static void bridge_signature_a(void* emu, uintptr_t fn) {
  (void)emu;
  (void)fn;
}

static void bridge_signature_b(void* emu, uintptr_t fn) {
  (void)emu;
  (void)fn;
}

static void check_rejected_unchanged(void* sym, box64_call_desc_t* d,
                                     box64_call_status_t want,
                                     const char* what) {
  box64_call_results_t out;
  box64_call_results_t before;
  memset(&out, 0xA5, sizeof(out));
  before = out;
  check_status(box64_call_desc(sym, d, &out), want, what);
  check(memcmp(&out, &before, sizeof(out)) == 0,
        "rejected descriptor does not write output");
}

int main(int argc, char** argv) {
  void* s_zero;
  void* s_all1;
  void* s_id;
  void* s_add2;
  void* s_sum6;
  void* s_invoke;
  void* s_probe;
  void* s_align;
  void* s_tail24;
  void* s_tail40;
  uint64_t gprs[6];
  box64_fpr_t fprs[8];
  uint8_t probe[80];
  uint8_t stack24[24];
  uint8_t stack40[40];
  uint64_t rax;
  int i;

  if (argc < 2) {
    printf("[call-unified] usage: %s <guest.so>\n", argv[0]);
    return 2;
  }
  if (box64_lib_init() != 0 || box64_ensure_emu() != 0) {
    printf("[call-unified] init failed: %s\n",
           box64_dlerror() ? box64_dlerror() : "?");
    return 1;
  }
  lib = box64_dlopen(argv[1], RTLD_NOW);
  if (!lib) {
    printf("[call-unified] dlopen failed: %s\n",
           box64_dlerror() ? box64_dlerror() : "?");
    return 1;
  }
  s_zero = guest_sym("g_retzero");
  s_all1 = guest_sym("g_id");
  s_id = s_all1;
  s_add2 = guest_sym("g_add2");
  s_sum6 = guest_sym("g_sum6");
  s_invoke = guest_sym("g_invoke");
  s_probe = guest_sym("probe_guest");
  s_align = guest_sym("align_probe");
  s_tail24 = guest_sym("stack_tail24");
  s_tail40 = guest_sym("stack_tail40");
  if (!s_zero || !s_all1 || !s_add2 || !s_sum6 || !s_invoke || !s_probe ||
      !s_align || !s_tail24 || !s_tail40)
    return 1;
  id_sym = s_id;

  /* Guest values, including both values that used to be ambiguous. */
  rax = 0xDEADBEEF;
  check_status(box64_call_simple(s_zero, 0, 0, 0, 0, 0, 0, &rax),
               BOX64_CALL_OK, "simple: guest return 0 has OK status");
  check(rax == 0, "simple: guest return 0 reaches rax_out");
  rax = 0;
  check_status(box64_call_simple(s_all1, UINT64_MAX, 0, 0, 0, 0, 0, &rax),
               BOX64_CALL_OK, "simple: guest return all ones has OK status");
  check(rax == UINT64_MAX, "simple: all ones return is preserved");

  /* Every simple arity is exercised; unused slots are intentionally zero. */
  check_status(box64_call_simple(s_zero, 0, 0, 0, 0, 0, 0, &rax),
               BOX64_CALL_OK, "simple: zero-slot call");
  check_status(box64_call_simple(s_all1, 7, 0, 0, 0, 0, 0, &rax),
               BOX64_CALL_OK, "simple: one-slot call");
  check_status(box64_call_simple(s_add2, 2, 3, 0, 0, 0, 0, &rax),
               BOX64_CALL_OK, "simple: two-slot call");
  check_status(box64_call_simple(s_sum6, 1, 2, 3, 0, 0, 0, &rax),
               BOX64_CALL_OK, "simple: three-slot call");
  check(rax == 6, "simple: three-slot value");
  check_status(box64_call_simple(s_sum6, 1, 2, 3, 4, 0, 0, &rax),
               BOX64_CALL_OK, "simple: four-slot call");
  check(rax == 10, "simple: four-slot value");
  check_status(box64_call_simple(s_sum6, 1, 2, 3, 4, 5, 0, &rax),
               BOX64_CALL_OK, "simple: five-slot call");
  check(rax == 15, "simple: five-slot value");
  check_status(box64_call_simple(s_sum6, 1, 2, 3, 4, 5, 6, &rax),
               BOX64_CALL_OK, "simple: six-slot call");
  check(rax == 21, "simple: six-slot value");

  /* Physical sparse masks and complete 128-bit SSE register copies. */
  memset(probe, 0, sizeof(probe));
  for (i = 0; i < 6; ++i) gprs[i] = 0xAAA0000000000000ULL + (uint64_t)i;
  gprs[0] = BOX64_PTR_TO_U64(probe);
  gprs[5] = 0x5555666677778888ULL;
  memset(fprs, 0, sizeof(fprs));
  fprs[1].lo = 0x1122334455667788ULL;
  fprs[1].hi = 0x99AABBCCDDEEFF00ULL;
  {
    box64_call_desc_t d = make_desc(0, 0x21, 0x02, 0, 0, gprs, fprs, NULL);
    check_status(box64_call_desc(s_probe, &d, NULL), BOX64_CALL_OK,
                 "descriptor: sparse physical masks accepted");
  }
  check(memcmp(probe + 0, &gprs[0], 8) == 0,
        "descriptor: sparse gpr bit 0 is not compressed");
  check(memcmp(probe + 40, &gprs[5], 8) == 0,
        "descriptor: sparse gpr bit 5 is not compressed");
  check(memcmp(probe + 64, &fprs[1], 16) == 0,
        "descriptor: fpr carries all 16 bytes");

  /* Unknown flags, reserved words, masks and missing arrays are rejected. */
  {
    box64_call_desc_t d = make_desc(1u << 8, 0, 0, 0, 0, NULL, NULL, NULL);
    check_rejected_unchanged(s_id, &d, BOX64_CALL_BAD_LAYOUT,
                             "descriptor: unknown flag is BAD_LAYOUT");
    d = make_desc(0, 0, 0, 0, 0, NULL, NULL, NULL);
    d.reserved0 = 1;
    check_rejected_unchanged(s_id, &d, BOX64_CALL_BAD_LAYOUT,
                             "descriptor: reserved0 is BAD_LAYOUT");
    d.reserved0 = 0;
    d.reserved1 = 1;
    check_rejected_unchanged(s_id, &d, BOX64_CALL_BAD_LAYOUT,
                             "descriptor: reserved1 is BAD_LAYOUT");
    d = make_desc(0, 0x40, 0, 0, 0, gprs, NULL, NULL);
    check_rejected_unchanged(s_id, &d, BOX64_CALL_BAD_LAYOUT,
                             "descriptor: gpr mask overflow is BAD_LAYOUT");
    d = make_desc(0, 0, 0x100, 0, 0, NULL, fprs, NULL);
    check_rejected_unchanged(s_id, &d, BOX64_CALL_BAD_LAYOUT,
                             "descriptor: fpr mask overflow is BAD_LAYOUT");
    d = make_desc(0, 0, 0, 0x10, 0, NULL, NULL, NULL);
    check_rejected_unchanged(s_id, &d, BOX64_CALL_BAD_LAYOUT,
                             "descriptor: return mask overflow is BAD_LAYOUT");
    d = make_desc(0, 1, 0, 0, 0, NULL, NULL, NULL);
    check_rejected_unchanged(s_id, &d, BOX64_CALL_BAD_ARGUMENT,
                             "descriptor: gpr mask without array is BAD_ARGUMENT");
    d = make_desc(0, 0, 1, 0, 0, NULL, NULL, NULL);
    check_rejected_unchanged(s_id, &d, BOX64_CALL_BAD_ARGUMENT,
                             "descriptor: fpr mask without array is BAD_ARGUMENT");
    d = make_desc(0, 0, 0, 0, 1, NULL, NULL, NULL);
    check_rejected_unchanged(s_id, &d, BOX64_CALL_BAD_ARGUMENT,
                             "descriptor: stack length without stack is BAD_ARGUMENT");
    check_rejected_unchanged(NULL, &d, BOX64_CALL_BAD_ARGUMENT,
                             "descriptor: NULL sym is BAD_ARGUMENT");
    check_status(box64_call_desc(s_id, NULL, NULL), BOX64_CALL_BAD_ARGUMENT,
                 "descriptor: NULL desc is BAD_ARGUMENT");
  }

  /* The public inline rule is what both box64_call_desc() and
   * winedl_call_desc() consume; assert its status split and reason reporting
   * directly so a drift is caught without going through the engine. */
  {
    const char* why = (const char*)0x1;
    box64_call_desc_t d = make_desc(0, 0, 0, 0, 0, NULL, NULL, NULL);
    check_status(box64_call_desc_validate(&d, &why), BOX64_CALL_OK,
                 "shared validate: minimal descriptor accepted");
    check(why == NULL, "shared validate: OK clears the reason");
    check_status(box64_call_desc_validate(NULL, &why), BOX64_CALL_BAD_ARGUMENT,
                 "shared validate: NULL descriptor is BAD_ARGUMENT");
    check(why != NULL, "shared validate: NULL descriptor reports a reason");
    d = make_desc(1u << 8, 0, 0, 0, 0, NULL, NULL, NULL);
    check_status(box64_call_desc_validate(&d, &why), BOX64_CALL_BAD_LAYOUT,
                 "shared validate: unknown flags is BAD_LAYOUT");
    d = make_desc(0, 0, 0, 0, 0, NULL, NULL, NULL);
    d.reserved1 = 1;
    check_status(box64_call_desc_validate(&d, &why), BOX64_CALL_BAD_LAYOUT,
                 "shared validate: reserved word is BAD_LAYOUT");
    d = make_desc(0, 0x40, 0x100, 0x10, 0, gprs, fprs, NULL);
    check_status(box64_call_desc_validate(&d, &why), BOX64_CALL_BAD_LAYOUT,
                 "shared validate: mask overflow is BAD_LAYOUT");
    d = make_desc(0, 0, 0, 0, 8, NULL, NULL, NULL);
    check_status(box64_call_desc_validate(&d, &why), BOX64_CALL_BAD_ARGUMENT,
                 "shared validate: stack without pointer is BAD_ARGUMENT");
    check_status(box64_call_desc_validate(&d, NULL), BOX64_CALL_BAD_ARGUMENT,
                 "shared validate: reason is optional");
  }

  /* No stack, 24-byte stack and 40-byte stack all enter with ABI alignment;
   * the assembly probes also prove rounded tails were explicitly zeroed. */
  {
    box64_call_results_t out;
    box64_call_desc_t d;
    uint64_t align_out = 0;
    uint64_t tail = UINT64_MAX;
    memset(stack24, 0xCC, sizeof(stack24));
    memset(stack40, 0xCC, sizeof(stack40));
    gprs[0] = BOX64_PTR_TO_U64(&align_out);
    d = make_desc(BOX64_CALL_FLAG_FORCE_FRAME, 1, 0,
                  BOX64_CALL_RET_RAX, 0, gprs, NULL, NULL);
    memset(&out, 0, sizeof(out));
    check_status(box64_call_desc(s_align, &d, &out), BOX64_CALL_OK,
                 "descriptor: FORCE_FRAME works with zero stack");
    check(out.rax == 8, "descriptor: zero-stack alignment");
    gprs[0] = BOX64_PTR_TO_U64(&tail);
    d = make_desc(0, 1, 0, BOX64_CALL_RET_RAX, 24, gprs, NULL, stack24);
    check_status(box64_call_desc(s_tail24, &d, &out), BOX64_CALL_OK,
                 "descriptor: 24-byte stack call");
    check(tail == 0, "descriptor: 24-byte stack tail is zero");
    tail = UINT64_MAX;
    d = make_desc(0, 1, 0, BOX64_CALL_RET_RAX, 40, gprs, NULL, stack40);
    check_status(box64_call_desc(s_tail40, &d, &out), BOX64_CALL_OK,
                 "descriptor: 40-byte stack call");
    check(tail == 0, "descriptor: 40-byte stack tail is zero");
  }

  /* A host function registered with two wrapper/signature identities must
   * receive two independent thunks.  This is the regression proof for the
   * AddCheckBridge deduplication key. */
  {
    void* cb_a = box64_callback_with((void*)reenter_host,
                                     (void*)bridge_signature_a);
    void* cb_b = box64_callback_with((void*)reenter_host,
                                     (void*)bridge_signature_b);
    check(cb_a != NULL && cb_b != NULL,
          "callback dedup: both wrapper registrations succeed");
    check(cb_a != cb_b,
          "callback dedup: same host with distinct wrappers gets distinct thunks");
  }

  /* A host callback re-enters through the new simple entry without mixing the
   * outer register file into the nested call. */
  {
    void* cb = box64_callback((void*)reenter_host);
    check(cb != NULL, "descriptor: callback bridge created");
    if (cb) {
      check_status(box64_call_simple(s_invoke, BOX64_PTR_TO_U64(cb), 7, 0, 0,
                                     0, 0, &rax),
                   BOX64_CALL_OK, "descriptor: reentrant simple call status");
      check(rax == 22, "descriptor: reentrant simple registers do not mix");
    }
  }

  if (failures) {
    printf("[call-unified] RESULT: FAIL (%d checks)\n", checks);
    return 1;
  }
  printf("[call-unified] RESULT: PASS (%d checks)\n", checks);
  return 0;
}
