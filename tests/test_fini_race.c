/*
 * Teardown/admission regression.
 *
 * Three properties are exercised against the real engine:
 *
 *   1. box64_lib_fini() called while a guest call is still in flight must
 *      REFUSE: the phase goes to the terminal FINAL state, the context is NOT
 *      freed underneath the running call, and the still-running call completes.
 *   2. A same-thread callback re-entry (the guest calls a host bridge that
 *      calls box64_call_simple again) nests correctly while the outer call is
 *      admitted.
 *   3. After the refusal, a new call fails with a clear error instead of
 *      entering the half-torn engine.
 *
 * The worker blocks inside a host callback (invoked by the guest through
 * g_invoke) until the main thread has run fini; the guest call then returns
 * normally, proving the context was left alive (no force free).
 */
#include <dlfcn.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "libbox64.h"

static int g_fail;
static int g_checks;

static void check(int cond, const char* what) {
  g_checks++;
  if (!cond) {
    g_fail = 1;
    printf("[fini-race] FAIL %s\n", what);
  } else {
    printf("[fini-race] ok   %s\n", what);
  }
}

static void* g_zero_sym;
static void* g_invoke_sym;
static void* g_cb_addr;

static pthread_mutex_t g_m = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static int g_entered;
static int g_release;
static int g_reentry_ok;

/* Host callback the guest reaches through g_invoke. Same-thread re-entry first,
 * then block until the main thread has attempted fini. */
static uint64_t blocking_cb(uint64_t x, uint64_t a1, uint64_t a2, uint64_t a3,
                            uint64_t a4, uint64_t a5) {
  uint64_t out = 0;
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;

  if (box64_call_simple(g_zero_sym, 0, 0, 0, 0, 0, 0, &out) == BOX64_CALL_OK &&
      out == 0x1234) {
    g_reentry_ok = 1;
  }

  pthread_mutex_lock(&g_m);
  g_entered = 1;
  pthread_cond_broadcast(&g_cv);
  while (!g_release) pthread_cond_wait(&g_cv, &g_m);
  pthread_mutex_unlock(&g_m);
  return x + 1;
}

static box64_call_status_t g_worker_status;
static uint64_t g_worker_rax;

static void* worker(void* arg) {
  (void)arg;
  g_worker_rax = 0;
  g_worker_status = box64_call_simple(
      g_invoke_sym, (uint64_t)(uintptr_t)g_cb_addr, 5, 0, 0, 0, 0,
      &g_worker_rax);
  return NULL;
}

int main(int argc, char** argv) {
  pthread_t th;
  void* handle;
  const char* why;

  if (argc < 2) {
    printf("[fini-race] usage: %s <libcall_guest.so>\n", argv[0]);
    return 2;
  }
  box64_diag_set(0);
  if (box64_lib_init() != 0) {
    printf("[fini-race] engine init: %s\n",
           box64_dlerror() ? box64_dlerror() : "?");
    return 1;
  }
  handle = box64_dlopen(argv[1], RTLD_NOW);
  if (!handle) {
    printf("[fini-race] dlopen: %s\n",
           box64_dlerror() ? box64_dlerror() : "?");
    return 1;
  }
  g_zero_sym = box64_dlsym(handle, "g_zero");
  g_invoke_sym = box64_dlsym(handle, "g_invoke");
  check(g_zero_sym != NULL && g_invoke_sym != NULL, "guest symbols resolved");
  g_cb_addr = box64_callback((void*)blocking_cb);
  check(g_cb_addr != NULL, "host callback bridged");

  if (pthread_create(&th, NULL, worker, NULL) != 0) {
    printf("[fini-race] pthread_create failed\n");
    return 1;
  }

  pthread_mutex_lock(&g_m);
  while (!g_entered) pthread_cond_wait(&g_cv, &g_m);
  pthread_mutex_unlock(&g_m);
  check(1, "worker is inside the guest call (blocked in the host callback)");
  check(box64_engine_phase() == 2, "phase READY while the worker runs");

  /* The refused teardown: a call is in flight, so this must NOT free. */
  box64_lib_fini();
  check(!box64_is_ready(), "fini refusal leaves the engine not ready");
  check(box64_engine_phase() == 5, "fini refusal publishes the terminal FINAL phase");
  why = box64_dlerror();
  check(why != NULL && strstr(why, "flight") != NULL,
        "fini refusal explains the in-flight calls");

  /* Post-refusal: new work must fail, and it must fail at admission (the
   * phase check), not inside a half-torn engine. */
  {
    uint64_t out = 0;
    box64_call_status_t st =
        box64_call_simple(g_zero_sym, 0, 0, 0, 0, 0, 0, &out);
    check(st != BOX64_CALL_OK, "call after refusal is not OK");
    why = box64_dlerror();
    check(why != NULL, "call after refusal records an error");
    box64_call_status_t st2 = box64_call_desc(g_zero_sym, NULL, NULL);
    check(st2 != BOX64_CALL_OK, "descriptor call after refusal refused");
  }

  /* Let the in-flight call finish. It must still succeed: the context was left
   * allocated by the refused teardown. */
  pthread_mutex_lock(&g_m);
  g_release = 1;
  pthread_cond_broadcast(&g_cv);
  pthread_mutex_unlock(&g_m);
  pthread_join(th, NULL);

  check(g_worker_status == BOX64_CALL_OK,
        "in-flight guest call completed after refused fini");
  check(g_worker_rax == 7, "in-flight guest call kept the live context (rax=7)");
  check(g_reentry_ok, "same-thread callback re-entry nested correctly");

  if (g_fail) {
    printf("[fini-race] RESULT: FAIL (%d checks)\n", g_checks);
    return 1;
  }
  printf("[fini-race] RESULT: PASS (%d checks)\n", g_checks);
  return 0;
}
