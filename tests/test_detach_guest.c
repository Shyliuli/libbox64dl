/*
 * White-box check for the host-side detach repair in libbox64_ext.h:
 *
 *   box64_prepare_detach_current_thread() / box64_detach_current_thread_guest()
 *   box64_detach_current_thread_guest_restore()
 *
 * It needs no Wine: a real box64 context + emu is created through the public
 * API, then the emu is put into the exact post-attach shape (RSP inside a
 * foreign stack mapping, GS base pointing at a foreign TEB) and the repair is
 * observed on the real fields.
 *
 * Asserted contract:
 *   1. the switch moves emu RSP off the foreign stack and onto the Box64
 *      emu stack (16-byte aligned top), and zeroes the guest GS binding;
 *   2. the pre-switch state is returned in the snapshot and restore() puts it
 *      back exactly (the failed-detach rollback);
 *   3. a thread without an emu is a success no-op (saved zeroed);
 *   4. an unusable emu stack fails closed: -1 and NOTHING is changed;
 *   5. saved == NULL is accepted as a one-way switch.
 *
 * Link: whole-archive libbox64.a + libdynarec.a + the Rust facade, exactly
 * like a host embedder, so thread_get_emu()/the emu layout are the real ones.
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

/* threads.c: the thread's own emu (creates a 256 KiB one if absent). We only
 * call it after box64_ensure_emu(), so it never fabricates here. */
extern x64emu_t* thread_get_emu(void);

static int g_fail;

static void check(int cond, const char* what) {
  printf("[detach-guest] %s %s\n", cond ? "ok  " : "FAIL", what);
  if (!cond) g_fail = 1;
}

static uint64_t rsp_of(x64emu_t* emu) { return emu->regs[_SP].q[0]; }
static void set_rsp(x64emu_t* emu, uint64_t v) { emu->regs[_SP].q[0] = v; }

/* Two page mappings standing in for the Wine PE stack and the TEB that Wine's
 * detach will free. They are deliberately outside the Box64 emu stack. */
static void* g_wine_stack;
static void* g_wine_teb;
static uint64_t g_wine_rsp;

static int in_range(uint64_t p, uint64_t lo, uint64_t hi) {
  return p >= lo && p < hi;
}

/* A thread that never touched box64_get/ensure_emu: no emu, so the repair
 * must be a success no-op. */
static void* no_emu_thread(void* arg) {
  box64_guest_state_t saved;
  int rc;
  (void)arg;
  memset(&saved, 0xAA, sizeof saved); /* prove it gets zeroed */
  rc = box64_prepare_detach_current_thread(&saved);
  check(rc == 0, "no-emu thread: prepare returns success (already host)");
  check(saved.rsp == 0 && saved.gs_base == 0 && saved.gs_sel == 0,
        "no-emu thread: snapshot zeroed");
  return NULL;
}

int main(void) {
  x64emu_t* emu;
  uint64_t lo, hi, top_expected;
  box64_guest_state_t saved;
  int rc;

  if (box64_lib_init() != 0) {
    printf("[detach-guest] FAIL box64_lib_init: %s\n", box64_dlerror());
    return 1;
  }
  if (box64_ensure_emu() != 0) {
    printf("[detach-guest] FAIL box64_ensure_emu: %s\n", box64_dlerror());
    return 1;
  }
  emu = thread_get_emu();
  if (!emu || !emu->init_stack || !emu->size_stack) {
    printf("[detach-guest] FAIL no usable emu\n");
    return 1;
  }
  lo = (uint64_t)(uintptr_t)emu->init_stack;
  hi = lo + (uint64_t)emu->size_stack;
  top_expected = (hi) & ~(uint64_t)0xF;
  printf("[detach-guest] box64 emu stack [%#llx,%#llx) top=%#llx\n",
         (unsigned long long)lo, (unsigned long long)hi,
         (unsigned long long)top_expected);

  g_wine_stack = mmap(NULL, 2 * 4096, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  g_wine_teb = mmap(NULL, 2 * 4096, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (g_wine_stack == MAP_FAILED || g_wine_teb == MAP_FAILED) {
    printf("[detach-guest] FAIL mmap stand-ins\n");
    return 1;
  }
  g_wine_rsp = (uint64_t)(uintptr_t)g_wine_stack + 4096 - 16;

  /* --- 1. the real repair ------------------------------------------ */
  set_rsp(emu, g_wine_rsp);
  emu->segs_offs[_GS] = (uint64_t)(uintptr_t)g_wine_teb;
  emu->segs[_GS] = 0;
  memset(&saved, 0, sizeof saved);
  rc = box64_prepare_detach_current_thread(&saved);
  check(rc == 0, "prepare returns 0 on a healthy emu");
  check(saved.rsp == g_wine_rsp, "snapshot kept the pre-switch RSP");
  check(saved.gs_base == (uint64_t)(uintptr_t)g_wine_teb,
        "snapshot kept the pre-switch GS base");
  check(rsp_of(emu) == top_expected, "RSP moved to the emu stack top");
  check(in_range(rsp_of(emu), lo, hi + 1),
        "RSP is inside the Box64 emu stack");
  check(!in_range(rsp_of(emu), (uint64_t)(uintptr_t)g_wine_stack,
                  (uint64_t)(uintptr_t)g_wine_stack + 2 * 4096),
        "RSP is NOT in the Wine stack stand-in");
  check((rsp_of(emu) & 0xF) == 0, "RSP is 16-byte aligned (push makes 8)");
  check(emu->segs_offs[_GS] == 0 && emu->segs[_GS] == 0,
        "guest GS binding cleared (signal_route sees host)");

  /* --- 2. rollback of a failed detach ------------------------------ */
  rc = box64_detach_current_thread_guest_restore(&saved);
  check(rc == 0, "restore returns 0");
  check(rsp_of(emu) == g_wine_rsp, "restore undid the RSP switch");
  check(emu->segs_offs[_GS] == (uint64_t)(uintptr_t)g_wine_teb,
        "restore undid the GS clear");

  /* --- 3. saved == NULL one-way switch ----------------------------- */
  rc = box64_detach_current_thread_guest(NULL);
  check(rc == 0, "one-way switch (NULL snapshot) returns 0");
  check(rsp_of(emu) == top_expected && emu->segs_offs[_GS] == 0,
        "one-way switch moved RSP and cleared GS");

  /* --- 4. fail closed: unusable emu stack -------------------------- */
  {
    void* keep = emu->init_stack;
    set_rsp(emu, g_wine_rsp);
    emu->segs_offs[_GS] = (uint64_t)(uintptr_t)g_wine_teb;
    emu->init_stack = NULL;
    rc = box64_prepare_detach_current_thread(&saved);
    check(rc == -1, "prepare fails when the emu stack is unusable");
    check(rsp_of(emu) == g_wine_rsp,
          "fail-closed: RSP was not touched");
    check(emu->segs_offs[_GS] == (uint64_t)(uintptr_t)g_wine_teb,
          "fail-closed: GS was not touched");
    emu->init_stack = keep;
  }

  /* --- 5. the alias has the same effect ---------------------------- */
  rc = box64_detach_current_thread_guest(&saved);
  check(rc == 0 && rsp_of(emu) == top_expected &&
            emu->segs_offs[_GS] == 0,
        "box64_detach_current_thread_guest alias matches prepare");

  /* --- 6. a thread with no emu is a no-op -------------------------- */
  {
    pthread_t th;
    if (pthread_create(&th, NULL, no_emu_thread, NULL) != 0) {
      check(0, "spawn no-emu thread");
    } else {
      pthread_join(th, NULL);
    }
  }

  if (g_fail) {
    printf("[detach-guest] RESULT: FAIL\n");
    return 1;
  }
  printf("[detach-guest] RESULT: PASS\n");
  return 0;
}
