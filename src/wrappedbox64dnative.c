/*
 * wrappedbox64dnative.c — engine-owned signature -> wrapper lookup.
 *
 * The native-module seam (include/libbox64_native_module.h) lets a provider
 * descriptor name guest symbols at run time. The engine bridges an ordinary
 * entry by resolving its host symbol with dlsym() and calling it through the
 * engine wrapper for the entry's signature token; a GOM entry is bridged to
 * its provider handler through the same wrapper. That means the engine has to
 * turn a run-time signature string into the compile-time wrapper function box64
 * generated for it.
 *
 * This file is that lookup, and it is the reason the seam does not carry a
 * second ABI rule source: the wrapper itself is box64's generated wrapper
 * (src/wrapped/generated/wrapper.c), selected by its own name. The signature
 * set the engine guarantees is declared once in
 * wrappedbox64dnative_private.h, which box64's own rebuild_wrappers.py also
 * parses (this file is a WRAPPEDS entry), so the declarations and the
 * definitions are generated from one source.
 *
 * Like the compiled-in wrapped tables, this file deliberately does not run
 * any signature logic itself.
 */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "libbox64_native_module.h"
#include "wrapper.h"

typedef void (*box64dnative_wrapper_t)(x64emu_t* emu, uintptr_t fnc);

typedef struct box64dnative_sig_s {
  const char* signature;
  box64dnative_wrapper_t wrapper;
} box64dnative_sig_t;

/* Build the table from the same token list the generator parsed. #W is the
 * signature token itself; W is the wrapper function the generator emitted
 * under that exact name (declared by wrapper.h above). */
#define DATA(N, S)
#define GO2(N, W, O)
#define GO(N, W) {#W, (box64dnative_wrapper_t)W},
#define GOM(N, W) {#W, (box64dnative_wrapper_t)W},
static const box64dnative_sig_t box64dnative_signatures[] = {
#include "wrappedbox64dnative_private.h"
};
#undef GO
#undef GOM
#undef GO2
#undef DATA

void* box64_native_module_wrapper(const char* signature) {
  size_t i;
  if (!signature || signature[0] == '\0') return NULL;
  for (i = 0; i < sizeof(box64dnative_signatures) / sizeof(box64dnative_signatures[0]); ++i) {
    if (strcmp(box64dnative_signatures[i].signature, signature) == 0)
      return (void*)box64dnative_signatures[i].wrapper;
  }
  return NULL;
}
