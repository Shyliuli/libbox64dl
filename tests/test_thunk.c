#include "box64dl_thunk.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define N_BIG 5000

void test_shim(void); /* from test_thunk_shim.S */
uint64_t test_dispatch(uint64_t tag, uint64_t arg);

uint64_t test_dispatch(uint64_t tag, uint64_t arg) { return tag * 1000 + arg; }

typedef uint64_t (*call_t)(uint64_t);

static int fails;

static void check(int cond, const char* what) {
  if (!cond) {
    printf("  FAIL %s\n", what);
    fails++;
  }
}

int main(void) {
  void* a;
  void* b;
  void* again;
  void** many;
  int i;

  /* 1. a thunk is callable and carries its data0 through */
  a = box64dl_thunk_get((void*)test_shim, 7, 0);
  check(a != NULL, "alloc");
  check(((call_t)a)(42) == 7042, "data0 bound, arg forwarded");

  /* 2. distinct data0 -> distinct thunk; same data0 -> same thunk */
  b = box64dl_thunk_get((void*)test_shim, 9, 0);
  check(b != a, "distinct data0 gets a distinct thunk");
  check(((call_t)b)(1) == 9001, "second thunk works");
  again = box64dl_thunk_get((void*)test_shim, 7, 0);
  check(again == a, "same data0 deduplicates");
  check(box64dl_thunk_peek((void*)test_shim, 9) == b, "peek finds it");
  check(box64dl_thunk_peek((void*)test_shim, 12345) == NULL, "peek misses");

  /* 3. release frees the slot for reuse and drops it from lookup */
  {
    size_t before = box64dl_thunk_count();
    box64dl_thunk_release(b);
    check(box64dl_thunk_count() == before - 1, "count drops on release");
    check(box64dl_thunk_peek((void*)test_shim, 9) == NULL, "released is gone");
    b = box64dl_thunk_get((void*)test_shim, 11, 0);
    check(((call_t)b)(2) == 11002, "recycled slot works");
  }

  /* 4. well past the old static ceiling of 256 */
  many = malloc(sizeof(void*) * N_BIG);
  for (i = 0; i < N_BIG; ++i) {
    many[i] = box64dl_thunk_get((void*)test_shim, (uint64_t)(1000 + i), 0);
    if (!many[i]) {
      printf("  FAIL alloc #%d\n", i);
      fails++;
      break;
    }
  }
  for (i = 0; i < N_BIG; ++i) {
    if (((call_t)many[i])((uint64_t)i) != (uint64_t)(1000 + i) * 1000 + i) {
      printf("  FAIL call #%d\n", i);
      fails++;
      break;
    }
  }
  printf("  %d thunks allocated and called (old static limit was 256)\n", N_BIG);

  /* 5. range release: what a guest dlclose would do */
  {
    size_t freed = box64dl_thunk_release_range(1000, 1000 + N_BIG);
    check(freed == N_BIG, "release_range frees the whole module's thunks");
    check(box64dl_thunk_peek((void*)test_shim, 1500) == NULL,
          "ranged-out thunk is gone");
    check(box64dl_thunk_peek((void*)test_shim, 7) == a,
          "thunks outside the range survive");
    check(((call_t)a)(5) == 7005, "survivor still callable");
  }

  free(many);
  printf(fails ? "test_thunk FAILED (%d)\n" : "test_thunk PASSED\n", fails);
  return fails ? 1 : 0;
}
