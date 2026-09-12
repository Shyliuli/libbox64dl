/*
 * White-box check for the clean-guest-stack repair in libbox64_ext.h:
 *
 *   box64_reset_current_thread_guest_stack()
 *   box64_current_thread_teb_stack_top()
 *   box64_reset_current_thread_guest_stack_from_teb()
 *
 * It needs no Wine: a real box64 context + emu is created through the public
 * API, then a fake TEB is installed as the emu's guest GS base and the reset is
 * observed on the real fields.
 *
 * Asserted contract:
 *   1. the TEB getter returns TIB.StackBase for a valid fake TEB;
 *   2. the reset points emu RSP at (StackBase & ~0xF) - 0x28: 16-byte top,
 *      RSP % 16 == 8, and the 0x28-byte slot+shadow window below the top is
 *      inside the stack region;
 *   3. the guest GS/TEB binding is preserved (unlike the detach switch);
 *   4. the from_teb convenience is the two combined;
 *   5. an unusable TEB fails closed: -1 and RSP untouched;
 *   6. a zero stack_top fails closed: -1 and RSP untouched;
 *   7. a thread with no emu is a success no-op for both spellings.
 *
 * Link: whole-archive libbox64.a + libdynarec.a + the Rust facade, exactly
 * like a host embedder, so thread_peek_emu()/the emu layout are the real ones.
 * The archive has no main(), so this file's main is the entry point.
 */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "libbox64.h"
#include "libbox64_ext.h"

#include "os.h"
#include "regs.h"
#include "x64emu.h"
#include "emu/x64emu_private.h"

/* threads.c: this thread's own emu, or NULL (never fabricates). */
extern x64emu_t* thread_peek_emu(void);

static int g_fail;

static void check(int cond, const char* what) {
  printf("[reset-stack] %s %s\n", cond ? "ok  " : "FAIL", what);
  if (!cond) g_fail = 1;
}

static uint64_t rsp_of(x64emu_t* emu) { return emu->regs[_SP].q[0]; }
static void set_rsp(x64emu_t* emu, uint64_t v) { emu->regs[_SP].q[0] = v; }

static int in_range(uint64_t p, uint64_t lo, uint64_t hi) {
  return p >= lo && p < hi;
}

/* A thread that never touched box64_get/ensure_emu: no emu, so the repair must
 * be a success no-op under either spelling. */
static void* no_emu_thread(void* arg) {
  int rc1, rc2;
  (void)arg;
  rc1 = box64_reset_current_thread_guest_stack(0x400000);
  rc2 = box64_reset_current_thread_guest_stack_from_teb();
  check(rc1 == 0, "no-emu thread: raw reset is a success no-op");
  check(rc2 == 0, "no-emu thread: from_teb reset is a success no-op");
  return NULL;
}

int main(void) {
  x64emu_t* emu;
  void* teb;
  uint64_t base, limit, top = 0, expect, before;
  uint64_t saved_gs, saved_sel;
  int rc;

  if (box64_lib_init() != 0) {
    printf("[reset-stack] FAIL box64_lib_init: %s\n", box64_dlerror());
    return 1;
  }
  if (box64_ensure_emu() != 0) {
    printf("[reset-stack] FAIL box64_ensure_emu: %s\n", box64_dlerror());
    return 1;
  }
  emu = thread_peek_emu();
  if (!emu) {
    printf("[reset-stack] FAIL no emu on the main thread\n");
    return 1;
  }

  /* --- fake Wine TEB + PE stack (2 pages) --------------------------- */
  teb = mmap(NULL, 2 * 4096, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (teb == MAP_FAILED) {
    printf("[reset-stack] FAIL mmap fake TEB\n");
    return 1;
  }
  limit = (uint64_t)(uintptr_t)teb;
  base = limit + 2 * 4096;
  *(uint64_t*)((uint8_t*)teb + 0x08) = base;  /* TIB.StackBase  */
  *(uint64_t*)((uint8_t*)teb + 0x10) = limit; /* TIB.StackLimit */
  expect = (base & ~(uint64_t)0xF) - 0x28;
  printf("[reset-stack] fake PE stack [%#llx,%#llx) anchor=%#llx\n",
         (unsigned long long)limit, (unsigned long long)base,
         (unsigned long long)expect);

  saved_gs = emu->segs_offs[_GS];
  saved_sel = emu->segs[_GS];
  emu->segs_offs[_GS] = (uint64_t)(uintptr_t)teb;
  emu->segs[_GS] = 0;

  /* --- 1. the TEB getter ------------------------------------------- */
  rc = box64_current_thread_teb_stack_top(&top);
  check(rc == 0, "teb getter returns 0 on a valid fake TEB");
  check(top == base, "teb getter returned TIB.StackBase");

  /* --- 2. the raw reset -------------------------------------------- */
  set_rsp(emu, 0); /* dirty it first */
  rc = box64_reset_current_thread_guest_stack(top);
  check(rc == 0, "raw reset returns 0 with a valid stack top");
  check(rsp_of(emu) == expect, "RSP parked at (top & ~0xF) - 0x28");
  check((rsp_of(emu) & 0xF) == 8, "RSP keeps the x86_64 entry parity (8)");
  check(in_range(rsp_of(emu), limit, base),
        "RSP is inside the guest stack region");
  check(in_range(rsp_of(emu) - 8, limit, base),
        "the pushed return slot below RSP is inside the region");
  check(in_range(rsp_of(emu) + 8, limit, base) &&
            in_range(rsp_of(emu) + 0x28 - 1, limit, base),
        "the 32-byte shadow window [RSP+8,RSP+0x28) is inside the region");

  /* --- 3. GS/TEB preserved ----------------------------------------- */
  check(emu->segs_offs[_GS] == (uint64_t)(uintptr_t)teb,
        "guest GS base (TEB) preserved by the reset");
  check(emu->segs[_GS] == 0, "guest GS selector preserved by the reset");

  /* --- 4. the from_teb convenience --------------------------------- */
  set_rsp(emu, 0);
  rc = box64_reset_current_thread_guest_stack_from_teb();
  check(rc == 0, "from_teb reset returns 0");
  check(rsp_of(emu) == expect, "from_teb reset parked RSP identically");
  check(emu->segs_offs[_GS] == (uint64_t)(uintptr_t)teb,
        "from_teb reset kept the TEB binding");

  /* --- 5. fail closed: unusable TEB -------------------------------- */
  *(uint64_t*)((uint8_t*)teb + 0x08) = limit;  /* StackBase < StackLimit */
  *(uint64_t*)((uint8_t*)teb + 0x10) = base;
  before = rsp_of(emu);
  top = 0;
  rc = box64_current_thread_teb_stack_top(&top);
  check(rc == -1, "teb getter fails on StackBase < StackLimit");
  check(top == 0, "teb getter leaves *top untouched on failure");
  rc = box64_reset_current_thread_guest_stack_from_teb();
  check(rc == -1, "from_teb reset fails on an unusable TEB");
  check(rsp_of(emu) == before, "fail-closed: RSP was not touched");

  /* --- 6. fail closed: zero stack top ------------------------------ */
  rc = box64_reset_current_thread_guest_stack(0);
  check(rc == -1, "raw reset rejects a zero stack top");
  check(rsp_of(emu) == before, "fail-closed: RSP still untouched");

  /* --- 7. a thread with no emu is a no-op -------------------------- */
  {
    pthread_t th;
    if (pthread_create(&th, NULL, no_emu_thread, NULL) != 0) {
      check(0, "spawn no-emu thread");
    } else {
      pthread_join(th, NULL);
    }
  }

  /* restore the emu GS binding for a clean teardown */
  emu->segs_offs[_GS] = saved_gs;
  emu->segs[_GS] = saved_sel;

  if (g_fail) {
    printf("[reset-stack] RESULT: FAIL\n");
    return 1;
  }
  printf("[reset-stack] RESULT: PASS\n");
  return 0;
}
