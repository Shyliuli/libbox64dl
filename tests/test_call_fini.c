/*
 * Lifetime guard regression: a call issued AFTER box64_lib_fini() must be
 * refused even though the calling thread previously had an emu. Rust owns the
 * phase (FINAL after fini), and the C call entry checks readiness before using
 * any cached emu pointer. Before fini the same entries must succeed, which this
 * test also checks, so a guard that refuses everything cannot pass.
 *
 * The guard is a sequential lifetime check, not synchronisation: this test
 * never races fini with calls on other threads.
 */
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "libbox64.h"
#include "call_compat.h"

#include "os.h"
#include "regs.h"
#include "x64emu.h"
#include "emu/x64emu_private.h"

extern x64emu_t* thread_peek_emu(void);

static int g_fail;
static int g_checks;

static void check(int cond, const char* what) {
  g_checks++;
  if (!cond) {
    g_fail = 1;
    printf("[call-fini] FAIL %s\n", what);
  } else {
    printf("[call-fini] ok   %s\n", what);
  }
}

static void check_u64(uint64_t got, uint64_t want, const char* what) {
  if (got != want)
    printf("[call-fini]      got=%#llx want=%#llx\n",
           (unsigned long long)got, (unsigned long long)want);
  check(got == want, what);
}

int main(int argc, char** argv) {
  void* lib;
  void* zero;
  uint64_t gprs[6];
  uint64_t fregs[8];
  box64_fpr_t pfprs[8];
  static const box64_sigplan_t plan = BOX64_SIGPLAN(0, 0, 1, 0);

  if (argc < 2) {
    printf("[call-fini] usage: %s <guest.so>\n", argv[0]);
    return 2;
  }
  box64_diag_set(0);
  if (box64_lib_init() != 0 || box64_ensure_emu() != 0) {
    printf("[call-fini] engine init: %s\n",
           box64_dlerror() ? box64_dlerror() : "?");
    return 1;
  }
  lib = box64_dlopen(argv[1], RTLD_NOW);
  if (!lib) {
    printf("[call-fini] dlopen: %s\n",
           box64_dlerror() ? box64_dlerror() : "?");
    return 1;
  }
  zero = box64_dlsym(lib, "g_zero");
  if (!zero) {
    printf("[call-fini] dlsym g_zero failed\n");
    return 1;
  }

  /* Before fini: the same entries work, and the thread has an emu. */
  check_u64(test_simple0(zero), 0x1234, "before fini: call0 runs the guest");
  check(box64_dlerror() == NULL, "before fini: error slot clear");
  check(thread_peek_emu() != NULL, "before fini: thread has an emu");

  /* Fini tears the engine down; the emu pointer on this thread goes stale. */
  box64_lib_fini();

  memset(gprs, 0, sizeof(gprs));
  memset(fregs, 0, sizeof(fregs));
  memset(pfprs, 0, sizeof(pfprs));

  check_u64(test_simple0(zero), 0, "after fini: call0 refused (returns 0)");
  check(box64_dlerror() != NULL, "after fini: call0 records an error");
  check(box64_dlerror() == NULL, "after fini: error slot is read-and-consume");

  check_u64(test_desc_regs(zero, gprs, fregs), 0,
            "after fini: descriptor_regs refused");
  check(box64_dlerror() != NULL, "after fini: descriptor_regs records an error");

  check_u64(test_desc_blob(zero, gprs, fregs, NULL, 0), 0,
            "after fini: descriptor blob refused");
  check(box64_dlerror() != NULL,
        "after fini: descriptor blob records an error");

  check_u64(test_desc_plan(zero, &plan, gprs, pfprs, NULL, NULL), 0,
            "after fini: descriptor layout refused");
  check(box64_dlerror() != NULL, "after fini: descriptor layout records an error");

  if (g_fail) {
    printf("[call-fini] RESULT: FAIL (%d checks)\n", g_checks);
    return 1;
  }
  printf("[call-fini] RESULT: PASS (%d checks)\n", g_checks);
  return 0;
}
