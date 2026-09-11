/* Embeddable box64 runtime — no main()/initialize() process takeover. */
#define _GNU_SOURCE
#include <assert.h>
#include "libbox64.h"

/* core.c::initialize references unittest(); provide a stub so libbox64
 * does not need test.c when embedded. */
int unittest(int argc, const char** argv) {
  (void)argc;
  (void)argv;
  return 1;
}

#include <dlfcn.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "box64dl_thunk.h"

#include "box64context.h"
#include "box64cpu.h"
#include "elfloader.h"
#include "library.h"
#include "box64cpu_util.h"
#include "box64stack.h"
#include "bridge.h"
#include "callback.h"
#include "custommem.h"
#include "debug.h"
#include "env.h"
#include "fileutils.h"
#include "hostext.h"
#include "librarian.h"
#include "pathcoll.h"
#include "sysinfo.h"
#include "threads.h"
#include "x64emu.h"
#include "x64tls.h"
#include "os.h"
#include "emu/x64emu_private.h"

extern box64context_t* my_context;
extern FILE* ftrace;
extern size_t box64_pagesize;

void LoadEnvVariables(void);
void InitializeEnvFiles(void);
void LoadLDPath(box64context_t* context);
void init_pthread_helper(void);
void endBox64(void);
void setupTraceInit(void);
void setupTrace(void);
void loadProtectionFromMap(void);

/* threads.c: returns NULL instead of auto-creating, unlike thread_get_emu(). */
emuthread_t* thread_get_et(void);

library_t* dl_get_library(void* handle);
void* my_dlopen(x64emu_t* emu, void* filename, int flag);
void* my_dlsym(x64emu_t* emu, void* handle, void* symbol);
int my_dlclose(x64emu_t* emu, void* handle);
char* my_dlerror(x64emu_t* emu);

/* Rust facade (rust/src/lib.rs)：线程局部错误槽，读到即消费。
 * box64_set_err(NULL) 清空；box64_get_error 的指针合法到本线程下一次错误操作。 */
extern void box64_set_err(const char* msg);
extern const char* box64_get_error(void);

/* The single recursive lock instance lives in the Rust facade (GLOBAL.ctx,
 * a PtMutex). Both sides go through pt_mutex (rust/src/pt_mutex/): the C
 * wrappers below take the pthread handle from box64_ctx_mutex(), Rust locks
 * the same PtMutex directly. box64_ctx_mutex() hands the handle out only
 * after the per-instance lazy init (double-checked, attr-based recursive
 * init in C) — no glibc _NP initializer involved. */
int pt_mutex_lock(pthread_mutex_t* m);   /* pt_mutex/pt_mutex.h */
int pt_mutex_unlock(pthread_mutex_t* m);
pthread_mutex_t* box64_ctx_mutex(void);  /* rust/src/lib.rs */

static void lock(void) { pt_mutex_lock(box64_ctx_mutex()); }
static void unlock(void) { pt_mutex_unlock(box64_ctx_mutex()); }

static void set_err(const char* msg) { box64_set_err(msg); }

/* ensure_emu, box64_lib_init and box64_is_ready live in the Rust facade
 * (rust/src/lib.rs); lib_init creates the context and the first emu,
 * ensure_emu guarantees a full-size stack per thread (thread_get_emu's
 * 256 KiB fallback is too small for recursive guests). The lock contract
 * is unchanged: entry points hold it across the call; wrapped callbacks
 * may call box64_ensure_emu unlocked — the recursive lock takes care of
 * both. */

BOX64_API void box64_lib_fini(void) {
  printf("[box64] box64_lib_fini enter.\n");
  lock();
  if (!box64_is_ready()) {
    unlock();
    return;
  }
  endBox64();
  my_context = NULL;
  unlock();
}

BOX64_API int box64_lib_ready(void) { return box64_is_ready(); }

BOX64_API void box64_set_wine_mode(void) {
  extern int box64_wine;
  box64_wine = 1;
}

BOX64_API void* box64_dlopen(const char* path, int flags) {
  x64emu_t* emu;
  void* handle;
  printf("[box64] box64_dlopen path=%s.\n", path ? path : "(null)");
  if (!box64_is_ready() && box64_lib_init() != 0) return NULL;
  if (!path) {
    set_err("box64_dlopen: NULL path");
    return NULL;
  }
  lock();
  if (box64_ensure_emu() != 0) {
    unlock();
    return NULL;
  }
  emu = thread_get_emu();
  if (flags == 0) flags = RTLD_NOW;
  /*core:dlopen的实际执行者*/
  handle = my_dlopen(emu, (void*)path, flags);
  if (!handle) {
    char* e = my_dlerror(emu);
    set_err(e ? e : "box64_dlopen failed");
  } else {
    set_err(NULL);
  }
  unlock();
  return handle;
}

BOX64_API void* box64_dlsym(void* handle, const char* name) {
  x64emu_t* emu;
  void* p;
  printf("[box64] box64_dlsym name=%s.\n", name ? name : "(null)");
  if (!box64_is_ready()) {
    set_err("not initialized");
    return NULL;
  }
  if (!name) {
    set_err("NULL symbol");
    return NULL;
  }
  lock();
  if (box64_ensure_emu() != 0) {
    unlock();
    return NULL;
  }
  emu = thread_get_emu();
  p = my_dlsym(emu, handle, (void*)name);
  if (!p) {
    char* e = my_dlerror(emu);
    set_err(e ? e : "symbol not found");
  } else {
    set_err(NULL);
  }
  unlock();
  return p;
}

BOX64_API int box64_dlclose(void* handle) {
  x64emu_t* emu;
  int rc;
  printf("[box64] box64_dlclose handle=%p.\n", handle);
  uintptr_t lo = 0, hi = 0;
  if (!box64_is_ready()) return -1;
  lock();
  if (box64_ensure_emu() != 0) {
    unlock();
    return -1;
  }
  emu = thread_get_emu();

  /* Note the guest's address range before closing so its trampolines can be
   * reclaimed. Thunks pointing into unmapped guest code are worse than leaked
   * ones: the host would jump into whatever gets mapped there next. */
  {
    library_t* lib = dl_get_library(handle);
    elfheader_t* elf = (lib && lib != (library_t*)-1LL) ? GetElf(lib) : NULL;
    if (elf) {
      lo = (uintptr_t)GetBaseAddress(elf);
      hi = lo + GetBaseSize(elf);
    }
  }

  rc = my_dlclose(emu, handle);

  /* dlclose only drops a reference; release only once the ELF is really gone. */
  if (hi > lo && !FindElfAddress(my_context, lo))
    box64dl_thunk_release_range(lo, hi);

  unlock();
  return rc;
}

BOX64_API const char* box64_dlerror(void) { return box64_get_error(); }
BOX64_API uint64_t box64_call_regs(void* sym, uint64_t* gregs, uint64_t* fregs) {
  x64emu_t* emu;
  int nn[] = {_DI, _SI, _DX, _CX, _R8, _R9};
  int i;
  uint64_t ret;
  printf("[box64] box64_call_regs sym=%p.\n", sym);
  if (!box64_is_ready() || !sym || !gregs || !fregs) return 0;
  lock();
  if (box64_ensure_emu() != 0) {
    unlock();
    return 0;
  }
  emu = thread_get_emu();
  unlock();

  for (i = 0; i < 6; ++i) emu->regs[nn[i]].q[0] = gregs[i];
  for (i = 0; i < 8; ++i) memcpy(&emu->xmm[i], &fregs[i], sizeof(uint64_t));

  /* emu and its stack are thread-local; guest execution runs unserialized. */
  DynaCall(emu, (uintptr_t)sym, 0);

  ret = R_RAX;
  gregs[0] = R_RAX;
  gregs[1] = R_RDX;
  memcpy(&fregs[0], &emu->xmm[0], sizeof(uint64_t));
  memcpy(&fregs[1], &emu->xmm[1], sizeof(uint64_t));
  return ret;
}

BOX64_API uint64_t box64_call_marshaled(void* sym, uint64_t* gregs,
                                        uint64_t* fregs, const void* stack,
                                        size_t stack_len) {
  x64emu_t* emu;
  int nn[] = {_DI, _SI, _DX, _CX, _R8, _R9};
  int i;
  uint64_t ret;
  uintptr_t oldip;
  printf("[box64] box64_call_marshaled sym=%p stack_len=%zu.\n", sym,
         stack_len);
  size_t al;
  if (!box64_is_ready() || !sym || !gregs || !fregs) return 0;
  lock();
  if (box64_ensure_emu() != 0) {
    unlock();
    return 0;
  }
  emu = thread_get_emu();
  unlock();

  Push64(emu, R_RBP);
  R_RBP = R_RSP;
  al = (stack_len + 15u) & ~15u;
  if (al) {
    R_RSP -= al;
    if (stack && stack_len)
      memcpy((void*)(uintptr_t)R_RSP, stack, stack_len);
    if (al > stack_len)
      memset((void*)(uintptr_t)(R_RSP + stack_len), 0, al - stack_len);
  }

  for (i = 0; i < 6; ++i) emu->regs[nn[i]].q[0] = gregs[i];
  for (i = 0; i < 8; ++i) memcpy(&emu->xmm[i], &fregs[i], sizeof(uint64_t));

  oldip = R_RIP;
  DynaCall(emu, (uintptr_t)sym, 0);
  if (oldip == R_RIP) {
    R_RSP = R_RBP;
    R_RBP = Pop64(emu);
  }

  ret = R_RAX;
  gregs[0] = R_RAX;
  gregs[1] = R_RDX;
  memcpy(&fregs[0], &emu->xmm[0], sizeof(uint64_t));
  memcpy(&fregs[1], &emu->xmm[1], sizeof(uint64_t));
  return ret;
}

/* Custom bridge wrapper: guest SysV args → host fn(a0..a5). */
static void bridge_host6(x64emu_t* emu, uintptr_t fnc) {
  typedef uint64_t (*hfn)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
                          uint64_t);
  hfn f = (hfn)fnc;
  (void)emu;
  R_RAX = f(R_RDI, R_RSI, R_RDX, R_RCX, R_R8, R_R9);
}

/*
 * Register-file access for a bridge callee, so callers can implement their own
 * argument decoding without depending on the emulator's layout. `emu` is the
 * opaque first parameter a bridge function receives.
 */
BOX64_API void box64_bridge_args(void* emuv, uint64_t* gregs, uint64_t* fregs) {
  x64emu_t* emu = (x64emu_t*)emuv;
  printf("[box64] box64_bridge_args emu=%p.\n", emuv);
  int nn[] = {_DI, _SI, _DX, _CX, _R8, _R9};
  int i;
  if (!emu || !gregs || !fregs) return;
  for (i = 0; i < 6; ++i) gregs[i] = emu->regs[nn[i]].q[0];
  for (i = 0; i < 8; ++i) memcpy(&fregs[i], &emu->xmm[i], sizeof(uint64_t));
}

BOX64_API void box64_bridge_ret(void* emuv, uint64_t rax, uint64_t rdx,
                                const uint64_t* fregs2) {
  x64emu_t* emu = (x64emu_t*)emuv;
  printf("[box64] box64_bridge_ret rax=0x%llx.\n",
         (unsigned long long)rax);
  if (!emu) return;
  R_RAX = rax;
  R_RDX = rdx;
  if (fregs2) {
    memcpy(&emu->xmm[0], &fregs2[0], sizeof(uint64_t));
    memcpy(&emu->xmm[1], &fregs2[1], sizeof(uint64_t));
  }
}

/* Bridge with the default six-integer-register wrapper. */
BOX64_API void* box64_callback(void* host_fn, const char* fmt) {
  (void)fmt;
  return box64_callback_with(host_fn, (void*)bridge_host6);
}

/*
 * Bridge `host_fn` through a caller-supplied wrapper, which receives the emu
 * and the host function pointer and is responsible for moving arguments and
 * the return value. Needed for callbacks whose parameters are not all
 * integers, where the six-register default cannot describe the signature.
 */
BOX64_API void* box64_callback_with(void* host_fn, void* bridge_fn) {
  uintptr_t br;
  printf("[box64] box64_callback_with host_fn=%p.\n", host_fn);
  if (!box64_is_ready() && box64_lib_init() != 0) return NULL;
  if (!host_fn || !bridge_fn) return NULL;
  lock();
  /* Check-first: AddBridge alone allocates a fresh brick per call, so
   * re-registering the same host function would leak bridge slots. */
  br = AddCheckBridge(my_context->system, (wrapper_t)bridge_fn, host_fn, 0,
                      NULL);
  unlock();
  return (void*)(uintptr_t)br;
}
