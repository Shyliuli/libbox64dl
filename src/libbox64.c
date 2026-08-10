/* Embeddable box64 runtime — no main()/initialize() process takeover. */
#define _GNU_SOURCE
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

static int g_ready;
static char g_err[512];
static pthread_mutex_t g_lock;
static pthread_once_t g_lock_once = PTHREAD_ONCE_INIT;

static void init_lock(void) {
  pthread_mutexattr_t a;
  pthread_mutexattr_init(&a);
  pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
  pthread_mutex_init(&g_lock, &a);
  pthread_mutexattr_destroy(&a);
}

static void lock(void) {
  pthread_once(&g_lock_once, init_lock);
  pthread_mutex_lock(&g_lock);
}

static void unlock(void) { pthread_mutex_unlock(&g_lock); }

static void set_err(const char* msg) {
  if (!msg) {
    g_err[0] = 0;
    return;
  }
  snprintf(g_err, sizeof(g_err), "%s", msg);
}

static void prepare_lib_paths(box64context_t* ctx) {
  LoadLDPath(ctx);
  AddPath(".", &ctx->box64_ld_lib, 1);
  AddPath("libssl.so.1", &ctx->box64_emulated_libs, 0);
  AddPath("libcrypto.so.1", &ctx->box64_emulated_libs, 0);
  AddPath("libunwind.so.8", &ctx->box64_emulated_libs, 0);
}

static int g_ctx_stack_taken;

/*
 * One emu per thread, each with its own guest stack.
 *
 * thread_get_emu() would auto-create an emu on a 256 KiB fallback stack, which
 * is too small for guest addons that recurse (tree-sitter, rust futures), so
 * probe with thread_get_et() and build the emu here instead. Callers hold the
 * global lock; the emu itself is thread-local and needs no serialization.
 */
static int ensure_emu(void) {
  x64emu_t* emu;
  uintptr_t stack;
  size_t stacksz;
  int own_stack;
  if (!my_context) return -1;
  if (thread_get_et()) return 0;

  if (!my_context->stack && CalcStackSize(my_context)) {
    set_err("CalcStackSize failed");
    return -1;
  }
  stacksz = my_context->stacksz ? my_context->stacksz : (8u << 20);

  if (!g_ctx_stack_taken && my_context->stack) {
    stack = (uintptr_t)my_context->stack;
    own_stack = 0;
    g_ctx_stack_taken = 1;
  } else {
    void* p = InternalMmap(NULL, stacksz, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS | MAP_GROWSDOWN, -1, 0);
    if (p == MAP_FAILED) {
      set_err("guest stack mmap failed");
      return -1;
    }
    setProtection_stack((uintptr_t)p, stacksz, PROT_READ | PROT_WRITE);
    stack = (uintptr_t)p;
    own_stack = 1;
  }

  emu = NewX64Emu(my_context,
                  my_context->ep ? my_context->ep : my_context->exit_bridge,
                  stack, stacksz, own_stack);
  if (!emu) {
    set_err("NewX64Emu failed");
    return -1;
  }
  SetupX64Emu(emu, NULL);
  refreshTLSData(emu);
  thread_set_emu(emu);
  return 0;
}

BOX64_API int box64_lib_init(void) {
  lock();
  if (g_ready) {
    unlock();
    return 0;
  }
  g_err[0] = 0;
  ftrace = stderr;
  box64_pagesize = (size_t)sysconf(_SC_PAGESIZE);
  if (!box64_pagesize) box64_pagesize = 4096;

  LoadEnvVariables();
  InitializeEnvFiles();
  if (!getenv("BOX64_NOBANNER")) setenv("BOX64_NOBANNER", "1", 0);

  InitializeSystemInfo();
#ifdef DYNAREC
  if (!DetectHostCpuFeatures()) SET_BOX64ENV(dynarec, 0);
#endif

  /*
   * Deliberately NOT calling init_signal_helper().
   *
   * It installs box64's SIGSEGV/SIGBUS/SIGILL/SIGABRT handlers with
   * sigaction(sig, &action, NULL) — the old handler is discarded, not chained.
   * Inside Electron that would destroy V8's own SIGSEGV handling (write
   * barriers, WASM traps), so the host keeps its handlers and box64 installs
   * none.
   *
   * Safe today because dynarec's write protection is a no-op for these guests:
   * protectDB() clears PROT_WRITE on pages holding translated code, but guest
   * .text is already r-x (no addon ships an RWE segment), so no fault can be
   * raised that box64 would have to service. Measured across a full merkle
   * index: zero mappings went from writable to read-only.
   *
   * It stops being safe for a guest with an RWE segment, one that mprotects its
   * own code writable at run time, or one that genuinely self-modifies. The fix
   * then is either CRC-based block validation (PROT_NEVERCLEAN / always_test,
   * no signals involved) or an embedded signal helper that saves the host
   * handlers and forwards anything box64 does not recognise.
   */

  my_context = NewBox64Context(1);
  if (!my_context) {
    set_err("NewBox64Context failed");
    unlock();
    return -1;
  }

  /*
   * NewBox64Context sets box64lib = dlopen(NULL), which is the host Node/Electron
   * executable when we are embedded in a .node addon. GOM my_* symbols live in
   * the addon DSO — rebind box64lib to the module that contains box64_lib_init.
   */
  {
    Dl_info dli;
    if (dladdr((void*)&box64_lib_init, &dli) && dli.dli_fname) {
      void* self = dlopen(dli.dli_fname, RTLD_NOW | RTLD_GLOBAL);
      if (self) my_context->box64lib = self;
    }
  }

  prepare_lib_paths(my_context);

  my_context->box64path = box_realpath("/proc/self/exe", NULL);
  if (!my_context->box64path) my_context->box64path = box_strdup("libbox64");
  my_context->argv[0] = box_strdup("libbox64");
  my_context->fullpath = box_strdup(my_context->argv[0]);
  my_context->envc = 0;
  my_context->envv = (char**)box_calloc(1, sizeof(char*));

  init_pthread_helper();
  loadProtectionFromMap();

  if (CalcStackSize(my_context)) {
    set_err("stack alloc failed");
    FreeBox64Context(&my_context);
    unlock();
    return -1;
  }

  my_context->ep = my_context->exit_bridge;
  if (ensure_emu() != 0) {
    FreeBox64Context(&my_context);
    unlock();
    return -1;
  }

  setupTraceInit();
  setupTrace();

  g_ready = 1;
  unlock();
  return 0;
}

BOX64_API void box64_lib_fini(void) {
  lock();
  if (!g_ready) {
    unlock();
    return;
  }
  endBox64();
  my_context = NULL;
  g_ready = 0;
  unlock();
}

BOX64_API int box64_lib_ready(void) { return g_ready; }

BOX64_API void* box64_dlopen(const char* path, int flags) {
  x64emu_t* emu;
  void* h;
  char buf[4096];
  const char* abs = path;
  if (!g_ready && box64_lib_init() != 0) return NULL;
  if (!path) {
    set_err("box64_dlopen: NULL path");
    return NULL;
  }
  lock();
  if (ensure_emu() != 0) {
    unlock();
    return NULL;
  }
  emu = thread_get_emu();
  if (path[0] != '/' && realpath(path, buf)) abs = buf;
  if (flags == 0) flags = RTLD_NOW;
  h = my_dlopen(emu, (void*)abs, flags);
  if (!h) {
    char* e = my_dlerror(emu);
    set_err(e ? e : "box64_dlopen failed");
  } else {
    set_err(NULL);
  }
  unlock();
  return h;
}

BOX64_API void* box64_dlsym(void* handle, const char* name) {
  x64emu_t* emu;
  void* p;
  if (!g_ready) {
    set_err("not initialized");
    return NULL;
  }
  if (!name) {
    set_err("NULL symbol");
    return NULL;
  }
  lock();
  if (ensure_emu() != 0) {
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
  uintptr_t lo = 0, hi = 0;
  if (!g_ready) return -1;
  lock();
  if (ensure_emu() != 0) {
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

BOX64_API const char* box64_dlerror(void) { return g_err[0] ? g_err : NULL; }

BOX64_API uint64_t box64_call(void* sym, const char* fmt, ...) {
  va_list ap;
  uint64_t ret = 0;
  if (!g_ready || !sym) return 0;
  lock();
  if (ensure_emu() != 0) {
    unlock();
    return 0;
  }
  unlock();
  va_start(ap, fmt);
  if (!fmt || !fmt[0]) {
    ret = RunFunctionFmt((uintptr_t)sym, "");
  } else if (strcmp(fmt, "i") == 0) {
    int a0 = va_arg(ap, int);
    ret = RunFunctionFmt((uintptr_t)sym, "i", a0);
  } else if (strcmp(fmt, "ii") == 0) {
    int a0 = va_arg(ap, int), a1 = va_arg(ap, int);
    ret = RunFunctionFmt((uintptr_t)sym, "ii", a0, a1);
  } else if (strcmp(fmt, "iii") == 0) {
    int a0 = va_arg(ap, int), a1 = va_arg(ap, int), a2 = va_arg(ap, int);
    ret = RunFunctionFmt((uintptr_t)sym, "iii", a0, a1, a2);
  } else if (strcmp(fmt, "p") == 0) {
    void* a0 = va_arg(ap, void*);
    ret = RunFunctionFmt((uintptr_t)sym, "p", a0);
  } else if (strcmp(fmt, "ip") == 0) {
    int a0 = va_arg(ap, int);
    void* a1 = va_arg(ap, void*);
    ret = RunFunctionFmt((uintptr_t)sym, "ip", a0, a1);
  } else if (strcmp(fmt, "pp") == 0) {
    void* a0 = va_arg(ap, void*);
    void* a1 = va_arg(ap, void*);
    ret = RunFunctionFmt((uintptr_t)sym, "pp", a0, a1);
  } else if (strcmp(fmt, "dd") == 0) {
    double a0 = va_arg(ap, double), a1 = va_arg(ap, double);
    ret = RunFunctionFmt((uintptr_t)sym, "dd", a0, a1);
  } else if (strcmp(fmt, "ff") == 0) {
    double a0 = va_arg(ap, double), a1 = va_arg(ap, double);
    ret = RunFunctionFmt((uintptr_t)sym, "ff", (float)a0, (float)a1);
  } else {
    set_err("box64_call: unsupported fmt (use box64_call_regs)");
    ret = 0;
  }
  va_end(ap);
  return ret;
}

BOX64_API uint64_t box64_call_regs(void* sym, uint64_t* gregs, uint64_t* fregs) {
  x64emu_t* emu;
  int nn[] = {_DI, _SI, _DX, _CX, _R8, _R9};
  int i;
  uint64_t ret;
  if (!g_ready || !sym || !gregs || !fregs) return 0;
  lock();
  if (ensure_emu() != 0) {
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
  size_t al;
  if (!g_ready || !sym || !gregs || !fregs) return 0;
  lock();
  if (ensure_emu() != 0) {
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
  int nn[] = {_DI, _SI, _DX, _CX, _R8, _R9};
  int i;
  if (!emu || !gregs || !fregs) return;
  for (i = 0; i < 6; ++i) gregs[i] = emu->regs[nn[i]].q[0];
  for (i = 0; i < 8; ++i) memcpy(&fregs[i], &emu->xmm[i], sizeof(uint64_t));
}

BOX64_API void box64_bridge_ret(void* emuv, uint64_t rax, uint64_t rdx,
                                const uint64_t* fregs2) {
  x64emu_t* emu = (x64emu_t*)emuv;
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
  if (!g_ready && box64_lib_init() != 0) return NULL;
  if (!host_fn || !bridge_fn) return NULL;
  lock();
  /* Check-first: AddBridge alone allocates a fresh brick per call, so
   * re-registering the same host function would leak bridge slots. */
  br = AddCheckBridge(my_context->system, (wrapper_t)bridge_fn, host_fn, 0,
                      NULL);
  unlock();
  return (void*)(uintptr_t)br;
}
