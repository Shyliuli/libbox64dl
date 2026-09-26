/*
 * DSO-linked guest consumer.
 *
 * Unlike the static test links (which pull --whole-archive engine objects into
 * the test binary and therefore see every global symbol directly), this program
 * links ONLY the shipped libbox64dl.so through pkg-config.  It is the shape a
 * downstream embedder uses, and the only one that proves the engine's own
 * dlsym(my_context->box64lib, "my_*") path works against the version-scripted
 * DSO export table.
 *
 * Observed:
 *   - the guest constructor runs at box64_dlopen();
 *   - guest printf writes to stdout;
 *   - guest fopen/fputs writes the marker file;
 *   - guest bsearch (host bsearch calling a guest comparator) returns the hit;
 *   - the guest destructor runs from endBox64() during box64_lib_fini().
 */
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libbox64.h"

static int fails;

static void check(int cond, const char* what) {
  if (cond) {
    printf("[consumer-dso] ok   %s\n", what);
  } else {
    printf("[consumer-dso] FAIL %s\n", what);
    fails++;
  }
}

static int file_contains(const char* path, const char* needle) {
  char buf[512];
  size_t n;
  FILE* f = fopen(path, "r");
  if (!f) return 0;
  n = fread(buf, 1, sizeof(buf) - 1, f);
  fclose(f);
  buf[n] = 0;
  return strstr(buf, needle) != NULL;
}

int main(int argc, char** argv) {
  static const char marker[] = "[guest] fopen ok\n";
  char out_path[512];
  void* handle;
  void* run;
  uint64_t rax = 0;
  box64_call_status_t status;

  if (argc < 2) {
    printf("[consumer-dso] usage: %s <libc_guest.so>\n", argv[0]);
    return 2;
  }
  snprintf(out_path, sizeof out_path, "%s.out", argv[1]);
  remove(out_path);

  if (box64_lib_init() != 0) {
    printf("[consumer-dso] FAIL init: %s\n",
           box64_dlerror() ? box64_dlerror() : "?");
    return 1;
  }

  /* The guest has a real DT_NEEDED libc.so.6; RTLD_NOW forces every wrapped
   * libc symbol to resolve at load time instead of lazily. */
  handle = box64_dlopen(argv[1], RTLD_NOW);
  if (!handle) {
    printf("[consumer-dso] FAIL dlopen: %s\n",
           box64_dlerror() ? box64_dlerror() : "?");
    box64_lib_fini();
    return 1;
  }
  check(1, "libc guest dlopen resolved all DT_NEEDED symbols");

  run = box64_dlsym(handle, "guest_run");
  check(run != NULL, "guest_run exported");

  if (run) {
    status = box64_call_simple(run, (uint64_t)(uintptr_t)out_path, 0, 0, 0, 0,
                               0, &rax);
    check(status == BOX64_CALL_OK, "guest_run call status OK");
    check(rax == 42, "guest_run returned 42 (printf/fopen/bsearch all ran)");
    check(file_contains(out_path, marker), "guest fopen wrote the marker file");
  }

  /* Destructor runs inside endBox64(); its marker append proves guest fini ran
   * without a forced free of a live context. */
  box64_lib_fini();
  check(file_contains(out_path, "[guest] dtor"), "guest destructor ran at fini");

  if (fails) {
    printf("[consumer-dso] RESULT: FAIL (%d)\n", fails);
    return 1;
  }
  printf("[consumer-dso] RESULT: PASS\n");
  return 0;
}
