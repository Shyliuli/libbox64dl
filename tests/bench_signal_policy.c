/*
 * Microbenchmark for the per-call signal-mask policy (patches/0005).
 *
 * Two paths in one binary:
 *   - global/default: the process-wide policy from BOX64DL_SIGNAL_POLICY (or
 *     the FAST built-in default);
 *   - per-library: --library-policy fast|compat calls
 *     box64_signal_policy_set_library() on the loaded guest, so the O(1) range
 *     lookup + per-thread cache path is exercised while the global default
 *     stays whatever the environment selected.
 *
 * Everything goes to STDERR (library traces go to STDOUT), so run with
 * `>/dev/null`. A one-line JSON summary can be written with
 * `--json <path>` (used by the perf worker; not a report file).
 *
 * Usage: bench_signal_policy <guest.so> [iterations]
 *                            [--library-policy fast|compat] [--json <path>]
 *
 * Phases:
 *   call0/g_retzero        the pure top-level call path the policy changes
 *   call2/g_add2           two real arguments
 *   call0/stats_off        counters off (default)
 *   call0/stats_on         counters on: the cost of the debug bookkeeping
 *   plan0/g_retzero        test_desc_plan() with an empty plan
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE /* clock_gettime, also via -D_GNU_SOURCE on the cc line */
#endif
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "libbox64.h"
#include "call_compat.h"
#include "libbox64_signal.h"

static uint64_t now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void* g_lib;

static void* sym(const char* name) { return box64_dlsym(g_lib, name); }

/* Result rows, also emitted as JSON when --json is given. */
typedef struct {
  const char* name;
  long calls;
  double total_ms;
  double ns_per_call;
} bench_row_t;

static bench_row_t g_rows[8];
static int g_nrows;

static void report(const char* name, long n, uint64_t t0, uint64_t t1,
                   volatile uint64_t sink) {
  double ms = (double)(t1 - t0) / 1e6;
  double ns = (double)(t1 - t0) / (double)n;
  fprintf(stderr, "RESULT %-22s calls=%ld total_ms=%.2f ns_per_call=%.2f\n",
          name, n, ms, ns);
  if (g_nrows < (int)(sizeof(g_rows) / sizeof(g_rows[0]))) {
    g_rows[g_nrows].name = name;
    g_rows[g_nrows].calls = n;
    g_rows[g_nrows].total_ms = ms;
    g_rows[g_nrows].ns_per_call = ns;
    g_nrows++;
  }
  (void)sink;
}

static int policy_from_arg(const char* s) {
  if (!strcmp(s, "compat")) return BOX64_SIGNAL_POLICY_COMPAT;
  if (!strcmp(s, "fast")) return BOX64_SIGNAL_POLICY_FAST;
  return -1;
}

static void write_json(const char* path, const char* env, int global,
                       int libpol, int effective) {
  FILE* f;
  int i;
  if (!path) return;
  f = fopen(path, "w");
  if (!f) {
    fprintf(stderr, "cannot write %s\n", path);
    return;
  }
  fprintf(f,
          "{\n  \"env\": \"%s\",\n  \"global_policy\": %d,\n"
          "  \"library_policy\": %d,\n  \"library_effective\": %d,\n"
          "  \"results\": [\n",
          env ? env : "", global, libpol, effective);
  for (i = 0; i < g_nrows; ++i)
    fprintf(f,
            "    {\"name\": \"%s\", \"calls\": %ld, \"total_ms\": %.3f, "
            "\"ns_per_call\": %.3f}%s\n",
            g_rows[i].name, g_rows[i].calls, g_rows[i].total_ms,
            g_rows[i].ns_per_call, (i + 1 < g_nrows) ? "," : "");
  fprintf(f, "  ]\n}\n");
  fclose(f);
  fprintf(stderr, "JSON %s\n", path);
}

int main(int argc, char** argv) {
  long n, i;
  volatile uint64_t sink = 0;
  uint64_t t0, t1;
  void *zero, *add2;
  box64_signal_stats_t st;
  static const box64_sigplan_t plan0 = BOX64_SIGPLAN(0, 0, 1, 0);
  uint64_t gprs[6] = {0};
  box64_fpr_t fprs[8];
  const char* env = getenv("BOX64DL_SIGNAL_POLICY");
  const char* json_path = NULL;
  int libpol = -1; /* -1: use the global/default path */
  int effective = -1;
  int expect_fast;

  if (argc < 2) {
    fprintf(stderr,
            "usage: %s <guest.so> [iterations] "
            "[--library-policy fast|compat] [--json <path>]\n",
            argv[0]);
    return 2;
  }
  n = (argc > 2 && argv[2][0] != '-') ? atol(argv[2]) : 200000;
  if (n <= 0) n = 200000;
  for (i = (argc > 2 && argv[2][0] != '-') ? 3 : 2; i < argc; ++i) {
    if (!strcmp(argv[i], "--library-policy") && i + 1 < argc) {
      libpol = policy_from_arg(argv[++i]);
      if (libpol < 0) {
        fprintf(stderr, "bad --library-policy (want fast|compat)\n");
        return 2;
      }
    } else if (!strcmp(argv[i], "--json") && i + 1 < argc) {
      json_path = argv[++i];
    } else {
      fprintf(stderr, "unknown argument: %s\n", argv[i]);
      return 2;
    }
  }
  /* FAST is the built-in default; COMPAT is selected explicitly. A per-library
   * override does not change the expected global. */
  expect_fast = !(env && env[0] == 'c');

  box64_diag_set(0);
  if (box64_lib_init() != 0 || box64_ensure_emu() != 0) {
    const char* e = box64_dlerror(); /* read once */
    fprintf(stderr, "engine init failed: %s\n", e ? e : "?");
    return 1;
  }
  if ((box64_signal_policy_get() == BOX64_SIGNAL_POLICY_FAST) != expect_fast) {
    fprintf(stderr, "POLICY MISMATCH: env and published global disagree\n");
    return 1;
  }

  g_lib = box64_dlopen(argv[1], RTLD_NOW);
  if (!g_lib) {
    const char* e = box64_dlerror();
    fprintf(stderr, "dlopen failed: %s\n", e ? e : "?");
    return 1;
  }
  if (libpol >= 0 &&
      box64_signal_policy_set_library(g_lib, libpol) != 0) {
    const char* e = box64_dlerror();
    fprintf(stderr, "set_library failed: %s\n", e ? e : "?");
    return 1;
  }
  effective = box64_signal_policy_get_library(g_lib);
  fprintf(stderr, "POLICY env=%s global=%d library=%d effective=%d\n",
          env ? env : "(unset)", box64_signal_policy_get(), libpol, effective);

  zero = sym("g_retzero");
  add2 = sym("g_add2");
  if (!zero || !add2) {
    fprintf(stderr, "missing guest symbols\n");
    return 1;
  }

  memset(fprs, 0, sizeof(fprs));
  for (i = 0; i < 2000; ++i) {
    sink += test_simple0(zero);
    sink += test_simple2(add2, 1, 2);
    sink += test_desc_plan(zero, &plan0, gprs, fprs, NULL, NULL);
  }

  t0 = now_ns();
  for (i = 0; i < n; ++i) sink += test_simple0(zero);
  t1 = now_ns();
  report("call0/g_retzero", n, t0, t1, sink);

  t0 = now_ns();
  for (i = 0; i < n; ++i) sink += test_simple2(add2, 1, 2);
  t1 = now_ns();
  report("call2/g_add2", n, t0, t1, sink);

  t0 = now_ns();
  for (i = 0; i < n; ++i)
    sink += test_desc_plan(zero, &plan0, gprs, fprs, NULL, NULL);
  t1 = now_ns();
  report("plan0/g_retzero", n, t0, t1, sink);

  /* Counter cost: off (default) vs on. */
  {
    long dn = n < 50000 ? n : 50000;
    box64_signal_stats_enable(0);
    t0 = now_ns();
    for (i = 0; i < dn; ++i) sink += test_simple0(zero);
    t1 = now_ns();
    report("call0/stats_off", dn, t0, t1, sink);

    box64_signal_stats_enable(1);
    t0 = now_ns();
    for (i = 0; i < dn; ++i) sink += test_simple0(zero);
    t1 = now_ns();
    report("call0/stats_on", dn, t0, t1, sink);
    box64_signal_stats_enable(0);
  }

  memset(&st, 0, sizeof(st));
  box64_signal_stats(&st);
  fprintf(stderr,
          "STATS saves=%lu skips=%lu restores=%lu publishes=%d\n",
          st.setjmp_saves, st.setjmp_skips, st.mask_restores, st.publishes);
  write_json(json_path, env, box64_signal_policy_get(), libpol, effective);
  fprintf(stderr, "DONE sink=%llu\n", (unsigned long long)sink);
  return 0;
}
