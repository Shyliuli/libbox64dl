/*
 * Real DT_NEEDED-libc x86_64 guest for the DSO consumer.
 *
 * Every libc entry it uses (printf, fopen/fputs/fclose/fflush, bsearch) is a
 * wrapped host-libc symbol: box64's librarian resolves it with
 * dlsym(my_context->box64lib, "my_<name>").  This fixture therefore only runs
 * when the final engine DSO exports its internal `my*` wrapper table, and it
 * only runs when linked against libc (no -nostdlib), so the image carries a
 * real DT_NEEDED libc.so.6.
 *
 * The constructor/destructor pin the guest init/fini path: the ctor runs when
 * box64_dlopen() loads the image and the dtor runs from endBox64() during
 * box64_lib_fini().
 */
#include <stdio.h>
#include <stdlib.h>

static FILE* g_log;

__attribute__((constructor)) static void guest_ctor(void) {
  printf("[guest] ctor\n");
  fflush(stdout);
}

__attribute__((destructor)) static void guest_dtor(void) {
  printf("[guest] dtor\n");
  fflush(stdout);
  if (g_log) {
    fputs("[guest] dtor\n", g_log);
    fclose(g_log);
    g_log = NULL;
  }
}

static int cmp_int(const void* a, const void* b) {
  int x = *(const int*)a;
  int y = *(const int*)b;
  return (x > y) - (x < y);
}

/* 42 only when every wrapped-libc path resolved and the guest executed. */
int guest_run(const char* out_path) {
  static const int sorted[5] = {1, 2, 3, 4, 5};
  const int* hit;

  printf("[guest] printf ok\n");
  fflush(stdout);

  if (!out_path) return 1;
  g_log = fopen(out_path, "w");
  if (!g_log) return 2;
  fputs("[guest] fopen ok\n", g_log);
  fflush(g_log);

  hit = (const int*)bsearch(&sorted[2], sorted, 5, sizeof(sorted[0]), cmp_int);
  if (!hit || *hit != 3) return 3;

  printf("[guest] bsearch ok\n");
  fflush(stdout);
  return 42;
}
