/*
 * Microbenchmark for the call path: the old register-file entries
 * (box64_call_regs / box64_call_marshaled) against the fixed-arity fast
 * entries (box64_call0..6), and diagnostics off (the new default) against
 * diagnostics on (the old unconditional per-call printf).
 *
 * Usage:  bench_call_arity <guest.so> [iterations]
 *
 * All results go to STDERR; the library's diagnostic traces go to STDOUT, so a
 * run with the diagnostics phase enabled can be redirected as
 *
 *   bench_call_arity guest.so 200000 >/dev/null 2>evidence/bench.log
 *
 * without losing the numbers. The callee is the same real x86_64 guest object
 * the regression test uses (tests/guest/call_guest.c), so every phase runs the
 * identical guest code and only the host-side call path changes.
 */
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "libbox64.h"

static uint64_t now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void* g_lib;

static void* sym(const char* name) { return box64_dlsym(g_lib, name); }

static void report(const char* name, long n, uint64_t t0, uint64_t t1,
                   volatile uint64_t sink) {
  double ns = (double)(t1 - t0) / (double)n;
  fprintf(stderr, "RESULT %-22s calls=%ld total_ms=%.2f ns_per_call=%.2f\n",
          name, n, (double)(t1 - t0) / 1e6, ns);
  (void)sink;
}

int main(int argc, char** argv) {
  long n;
  long i;
  volatile uint64_t sink = 0;
  uint64_t gregs[6], fregs[8], mf[8];
  void* zero;
  void* add2;
  uint64_t t0, t1;

  if (argc < 2) {
    fprintf(stderr, "usage: %s <guest.so> [iterations]\n", argv[0]);
    return 2;
  }
  n = (argc > 2) ? atol(argv[2]) : 200000;
  if (n <= 0) n = 200000;

  box64_diag_set(0); /* phase-level control below; default stays off */
  if (box64_lib_init() != 0 || box64_ensure_emu() != 0) {
    fprintf(stderr, "engine init failed: %s\n",
            box64_dlerror() ? box64_dlerror() : "?");
    return 1;
  }
  g_lib = box64_dlopen(argv[1], RTLD_NOW);
  if (!g_lib) {
    fprintf(stderr, "dlopen %s failed: %s\n", argv[1],
            box64_dlerror() ? box64_dlerror() : "?");
    return 1;
  }
  zero = sym("g_retzero");
  add2 = sym("g_add2");
  if (!zero || !add2) {
    fprintf(stderr, "missing guest symbols\n");
    return 1;
  }

  for (i = 0; i < 6; ++i) gregs[i] = (uint64_t)(i + 1);
  for (i = 0; i < 8; ++i) {
    fregs[i] = 0;
    mf[i] = 0;
  }

  /* warm up code paths / dynarec blocks */
  for (i = 0; i < 2000; ++i) {
    sink += box64_call0(zero);
    sink += box64_call2(add2, 1, 2);
    sink += box64_call_regs(zero, gregs, fregs);
    sink += box64_call_marshaled(zero, gregs, mf, NULL, 0);
  }

  /* 1. same 0-arg callee: fixed-arity vs full register file. */
  t0 = now_ns();
  for (i = 0; i < n; ++i) sink += box64_call0(zero);
  t1 = now_ns();
  report("call0/g_retzero", n, t0, t1, sink);

  t0 = now_ns();
  for (i = 0; i < n; ++i) sink += box64_call_regs(zero, gregs, fregs);
  t1 = now_ns();
  report("call_regs/g_retzero", n, t0, t1, sink);

  t0 = now_ns();
  for (i = 0; i < n; ++i) sink += box64_call_marshaled(zero, gregs, mf, NULL, 0);
  t1 = now_ns();
  report("call_marshaled/g_retzero", n, t0, t1, sink);

  /* 2. two real arguments, resetting the file each iteration for call_regs so
   * the two phases run the same guest work. */
  t0 = now_ns();
  for (i = 0; i < n; ++i) sink += box64_call2(add2, 1, 2);
  t1 = now_ns();
  report("call2/g_add2", n, t0, t1, sink);

  t0 = now_ns();
  for (i = 0; i < n; ++i) {
    gregs[0] = 1;
    gregs[1] = 2;
    gregs[2] = gregs[3] = gregs[4] = gregs[5] = 0;
    sink += box64_call_regs(add2, gregs, fregs);
  }
  t1 = now_ns();
  report("call_regs/g_add2", n, t0, t1, sink);

  /* 3. diagnostics off (default) vs on (the historical per-call printf). The
   * traced entry is box64_call_regs(); the fixed-arity entries never trace, so
   * this is the cost the unconditional printf used to add to every call. The
   * stdout stream is expected to be redirected to /dev/null for this run. */
  {
    long dn = n < 20000 ? n : 20000;
    box64_diag_set(0);
    t0 = now_ns();
    for (i = 0; i < dn; ++i) sink += box64_call_regs(zero, gregs, fregs);
    t1 = now_ns();
    report("call_regs/diag_off", dn, t0, t1, sink);

    box64_diag_set(1);
    t0 = now_ns();
    for (i = 0; i < dn; ++i) sink += box64_call_regs(zero, gregs, fregs);
    t1 = now_ns();
    box64_diag_set(0);
    report("call_regs/diag_on", dn, t0, t1, sink);
  }

  fprintf(stderr, "DONE sink=%llu\n", (unsigned long long)sink);
  return 0;
}
