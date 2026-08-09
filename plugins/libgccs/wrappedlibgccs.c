#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define _GNU_SOURCE
#include <dlfcn.h>

#include "wrappedlibs.h"
#include "debug.h"
#include "wrapper.h"
#include "bridge.h"
#include "librarian/library_private.h"
#include "x64emu.h"
#include "box64context.h"

const char* libgccsName = "libgcc_s.so.1";
#define ALTNAME "libgcc_s.so"
#define LIBNAME libgccs

#ifndef STATICBUILD
#define PRE_INIT                                                            \
  if (1) {                                                                  \
    lib->w.lib = dlopen("libgcc_s.so.1", RTLD_LAZY | RTLD_GLOBAL);          \
    if (!lib->w.lib)                                                        \
      lib->w.lib = dlopen(NULL, RTLD_LAZY | RTLD_GLOBAL);                   \
  } else
#endif

#include "wrappedlib_init.h"
