/*
 * Consumer smoke test: use ONLY the public header and the shipped DSO.
 *
 * This is the downstream C embedding shape: link -lbox64dl, include
 * <libbox64.h>, drive the lifecycle. It also pins the E1 final-shutdown
 * contract: after box64_lib_fini() the engine is FINAL and a later
 * box64_lib_init() must be refused.
 */
#include <stdio.h>

#include "libbox64.h"

int main(void) {
  int fails = 0;

  if (box64_lib_init() != 0) {
    printf("[consumer] FAIL init: %s\n",
           box64_dlerror() ? box64_dlerror() : "?");
    return 1;
  }
  if (!box64_is_ready()) {
    printf("[consumer] FAIL not ready after init\n");
    fails++;
  }

  box64_lib_fini();
  if (box64_is_ready()) {
    printf("[consumer] FAIL still ready after fini\n");
    fails++;
  }
  /* FINAL is terminal: re-init must be refused. */
  if (box64_lib_init() == 0) {
    printf("[consumer] FAIL re-init accepted after final shutdown\n");
    fails++;
  } else {
    const char* why = box64_dlerror();
    if (!why) {
      printf("[consumer] FAIL re-init refused without a reason\n");
      fails++;
    }
  }

  if (fails) {
    printf("[consumer] RESULT: FAIL (%d)\n", fails);
    return 1;
  }
  printf("[consumer] RESULT: PASS (init, ready, fini, terminal re-init refusal)\n");
  return 0;
}
