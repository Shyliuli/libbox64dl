/*
 * core_shim.c — private mechanical glue between the Rust lifecycle owner and
 * the vendored box64 engine. See src/core_shim.h for the ownership contract.
 *
 * This file is compiled by the Rust crate's build script (together with
 * pt_mutex.c) and linked into the same final artifact as the Rust code. It is
 * NOT grafted into the vendored tree.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "core_shim.h"

#include "libbox64_ext.h" /* box64_set_box64path: public, defined here */

#include "auxval.h"
#include "box64context.h"
#include "custommem.h"
#include "debug.h"
#include "env.h"
#include "hostext.h"
#include "libbox64_signal.h"
/* The process-wide signal-action dispatcher (src/signal_actions.c). Its install
 * is owned by the engine's init_signal_helper (patches/0020); the shim only
 * reads whether that install succeeded. See box64dl_core_context_new(). */
#include "libbox64_signal_actions.h"
#include "os.h"
#include "pathcoll.h"
#include "regs.h"
#include "sysinfo.h"
#include "threads.h"
#include "x64emu.h"
#include "x64tls.h"

#include "emu/x64emu_private.h"

/* Rust-provided error slot (rust/src/error.rs). NULL clears it. */
void box64_set_err(const char* msg);

/* box64 core.c: full engine teardown; frees the context exactly once. Not
 * declared in a vendor header, so declare it here like libbox64.c does. */
void endBox64(void);
void LoadLDPath(box64context_t* context);
void setupTraceInit(void);
void setupTrace(void);
void openFTrace(void);

extern box64context_t* my_context;
extern FILE* ftrace; /* box64 debug.h tracks the trace stream */

/* threads.c: per-thread probes. thread_get_emu() fabricates a 256 KiB-stack emu
 * when the thread has none, so the shim exposes the non-fabricating probes to
 * Rust and keeps that fallback out of the Rust path. */
x64emu_t* thread_get_emu(void);
x64emu_t* thread_peek_emu(void); /* patch 0003: never fabricates */
emuthread_t* thread_get_et(void);
void thread_set_emu(x64emu_t* emu);

extern box64env_t box64env;

/* ------------------------------------------------------------------ */
/* environment bootstrap                                               */

void box64dl_core_boot_env(void) {
  /* page size: box64's own global, read by ALIGN() and the stack checks. */
  {
    long ps = sysconf(_SC_PAGESIZE);
    box64_pagesize = (ps > 0) ? (uintptr_t)ps : 4096u;
  }
  LoadEnvVariables();
  InitializeEnvFiles();
  if (getenv("BOX64_NOBANNER") == NULL) setenv("BOX64_NOBANNER", "1", 0);
  /* trace: ftrace defaults to stderr; openFTrace only overrides it when the
   * corresponding environment variable is set. */
  ftrace = stderr;
  openFTrace();
  setupTraceInit();
  setupTrace();
  InitializeSystemInfo();
#ifdef DYNAREC
  if (!DetectHostCpuFeatures()) SET_BOX64ENV(dynarec, 0);
#endif
}

int box64dl_core_signal_policy_publish(void) {
  return box64dl_signal_policy_publish();
}

const char* box64dl_core_signal_policy_refusal(void) {
  return box64dl_signal_policy_refusal();
}

/* ------------------------------------------------------------------ */
/* context lifecycle                                                   */

/* The signal-action module's liveness probe (src/signal_actions.c). Declared
 * WEAK on purpose: the module is grafted into the engine only once the patch
 * queue carries its engine half (patches/0020/0021), and until then this object
 * — which the Rust crate compiles into liblibbox64.a — must still link without
 * pulling in a definition that does not exist yet. A weak undefined reference
 * resolves to NULL in that window, which box64dl_core_context_new() reads as
 * "this build has no signal-action module" and skips the gate for. Once
 * src/signal_actions.c IS an ELFLOADER source, the definition is in the same
 * link and the gate is live. */
extern int box64dl_signal_actions_active(void) __attribute__((weak));

int box64dl_core_context_new(void) {
  /* Embedded context: argc = 1 (we run shared objects, not a main program).
   * NewBox64Context installs the four box64 signal handlers via
   * init_signal_helper; FreeBox64Context restores the pre-box64 host actions
   * again (patches 0003/0006). */
  my_context = NewBox64Context(1);
  if (!my_context) return -1;

  /* The signal-action dispatcher installs itself from inside
   * init_signal_helper (patches/0020), i.e. during NewBox64Context above, and
   * that install is TRANSACTIONAL: it reports success only when all four
   * engine signals (SIGSEGV/SIGBUS/SIGILL/SIGABRT) were captured and
   * re-dispatched, and rolls every partial install back otherwise
   * (src/signal_actions.c). The outcome is observable only after the context
   * exists, so it is checked here, and STARTUP IS GATED ON IT: an inactive
   * module reports ENOSYS from every action entry point, leaving the process
   * with a half-installed fatal-signal route. Fail loudly instead and release
   * the context exactly once — the caller does not free it on this path. */
  if (box64dl_signal_actions_active && !box64dl_signal_actions_active()) {
    box64_set_err("box64: the signal-action dispatcher could not be installed "
                  "(init_signal_helper's transactional install refused, so the "
                  "engine's fatal-signal route would be half-installed)");
    FreeBox64Context(&my_context);
    return -1;
  }
  return 0;
}

int box64dl_core_rebind(const void* box64_fn) {
  Dl_info dli;
  void* self;
  if (!my_context || !box64_fn) return -1;
  if (!dladdr(box64_fn, &dli) || !dli.dli_fname) return -1;
  self = dlopen(dli.dli_fname, RTLD_NOW | RTLD_GLOBAL);
  if (self == NULL) {
    /* A PIE main program cannot dlopen its own path (glibc refuses); fall back
     * to the host process itself. For executable consumers this is exactly
     * right: the GOM my_* symbols live in the -rdynamic main program. */
    self = dlopen(NULL, RTLD_NOW | RTLD_GLOBAL);
  }
  if (self == NULL) return -1;
  my_context->box64lib = self;
  return 0;
}

/* ------------------------------------------------------------------ */
/* embedded guest environment (immortal storage)                       */

/* auxv tags (guest-visible constants, same values as the host ABI) */
#define SHIM_AT_PAGESZ 6
#define SHIM_AT_UID 11
#define SHIM_AT_EUID 12
#define SHIM_AT_GID 13
#define SHIM_AT_EGID 14
#define SHIM_AT_PLATFORM 15
#define SHIM_AT_HWCAP 16
#define SHIM_AT_CLKTCK 17
#define SHIM_AT_SECURE 23
#define SHIM_AT_RANDOM 25
#define SHIM_AT_HWCAP2 26
#define SHIM_AT_EXECFN 31

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* Static storage: the embedded context is a process-wide singleton, and statics
 * survive a failed init on this process's lifetime. */
static uintptr_t s_auxv[32]; /* tag/value pairs, AT_NULL-terminated */
static char s_platform[] = "x86_64";
static unsigned char s_at_random[16];
static char s_orig_argv0[256]; /* my_prctl's strcpy target */
static char* s_orig_argv[2] = {s_orig_argv0, NULL};
static char* s_box64path; /* box64_set_box64path's immortal copy */

static void setup_guest_auxv(box64context_t* ctx) {
  const unsigned long hwcap = 1UL << 0 | 1UL << 4 | 1UL << 8 | 1UL << 11 |
                              1UL << 15 | 1UL << 19 | 1UL << 23 | 1UL << 24 |
                              1UL << 25 | 1UL << 26 | 1UL << 28 | 1UL << 30;
  uintptr_t p_random = (uintptr_t)real_getauxval(SHIM_AT_RANDOM);
  unsigned long clktck = real_getauxval(SHIM_AT_CLKTCK);
  int n = 0, i;
  if (!p_random) {
    for (i = 0; i < (int)sizeof(s_at_random); ++i)
      s_at_random[i] = (unsigned char)(random() & 0xff);
    p_random = (uintptr_t)s_at_random;
  }
#define AUX(tag, val) do { s_auxv[n++] = (tag); s_auxv[n++] = (val); } while (0)
  AUX(SHIM_AT_PAGESZ, box64_pagesize ? box64_pagesize : 4096);
  AUX(SHIM_AT_UID, (uintptr_t)real_getauxval(SHIM_AT_UID));
  AUX(SHIM_AT_EUID, (uintptr_t)real_getauxval(SHIM_AT_EUID));
  AUX(SHIM_AT_GID, (uintptr_t)real_getauxval(SHIM_AT_GID));
  AUX(SHIM_AT_EGID, (uintptr_t)real_getauxval(SHIM_AT_EGID));
  AUX(SHIM_AT_PLATFORM, (uintptr_t)s_platform);
  AUX(SHIM_AT_HWCAP, (uintptr_t)hwcap);
  AUX(SHIM_AT_CLKTCK, clktck ? clktck : 100);
  AUX(SHIM_AT_SECURE, (uintptr_t)real_getauxval(SHIM_AT_SECURE));
  AUX(SHIM_AT_RANDOM, p_random);
  AUX(SHIM_AT_HWCAP2, (uintptr_t)(1UL << 1));
  AUX(SHIM_AT_EXECFN, (uintptr_t)ctx->argv[0]);
#undef AUX
  for (i = n; i < (int)(sizeof(s_auxv) / sizeof(s_auxv[0])); ++i) s_auxv[i] = 0;
  ctx->auxval_start = s_auxv;
}

static char* probe_in_path(const char* name) {
  const char* path_env = getenv("PATH");
  const char* p;
  char buf[PATH_MAX];
  if (!path_env) return NULL;
  for (p = path_env;;) {
    const char* col = strchr(p, ':');
    size_t len = col ? (size_t)(col - p) : strlen(p);
    if (len && len + strlen(name) + 2 <= sizeof(buf)) {
      snprintf(buf, sizeof(buf), "%.*s/%s", (int)len, p, name);
      if (access(buf, X_OK) == 0) return box_strdup(buf);
    }
    if (!col) break;
    p = col + 1;
  }
  return NULL;
}

/* Packaged box64 shipped with this engine: next to the engine DSO (flat
 * layout) or in ../bin (runtime/lib + runtime/bin). NULL when absent. */
static char* probe_packaged_box64(void) {
  static const char* const rel[] = {"box64", "../bin/box64"};
  Dl_info dli;
  char dso[PATH_MAX], buf[PATH_MAX];
  char* slash;
  size_t i;
  if (!dladdr((void*)probe_packaged_box64, &dli) || !dli.dli_fname) return NULL;
  if (!realpath(dli.dli_fname, dso)) return NULL;
  slash = strrchr(dso, '/');
  if (!slash) return NULL;
  *slash = 0;
  for (i = 0; i < sizeof rel / sizeof *rel; ++i)
    if (snprintf(buf, sizeof buf, "%s/%s", dso, rel[i]) < (int)sizeof buf &&
        access(buf, X_OK) == 0)
      return box_realpath(buf, NULL);
  return NULL;
}

/* The guest's "own program" is the host process: box64's /proc/self/{exe,
 * cmdline} wrappers answer from fullpath and argv, so give them the host's
 * real values (argv[0] = fullpath, as standalone box64 resolves it). */
static void set_host_program(box64context_t* ctx) {
  char* args = NULL;
  size_t len = 0, cap = 0, i;
  int argc = 0, k;
  char** argv;
  FILE* f;

  ctx->fullpath = box_realpath("/proc/self/exe", NULL);
  if (!ctx->fullpath) ctx->fullpath = box_strdup("/proc/self/exe");

  f = fopen("/proc/self/cmdline", "rb");
  if (f) {
    for (;;) {
      size_t n;
      if (len + 1 >= cap) {
        char* p = (char*)box_realloc(args, cap = cap ? cap * 2 : 4096);
        if (!p) break;
        args = p;
      }
      n = fread(args + len, 1, cap - len - 1, f);
      if (!n) break;
      len += n;
    }
    fclose(f);
  }
  if (args) args[len] = 0;
  for (i = 0; i < len; ++i)
    if (!args[i]) ++argc;
  if (argc < 1) argc = 1;

  argv = (char**)box_calloc(argc + 1, sizeof(char*));
  if (!argv) return; /* keep NewBox64Context's one-slot argv */
  argv[0] = box_strdup(ctx->fullpath);
  for (i = 0, k = 0; args && i < len && k < argc; ++k) {
    if (k) argv[k] = args + i;
    i += strlen(args + i) + 1;
  }
  box_free(ctx->argv);
  ctx->argv = argv;
  ctx->argc = argc;
}

/* Host executable directory (symlinks resolved); -1 on failure. */
static int exe_dir(char* out, size_t out_len) {
  ssize_t n = readlink("/proc/self/exe", out, out_len - 1);
  char* slash;
  if (n <= 0) return -1;
  out[n] = 0;
  slash = strrchr(out, '/');
  if (!slash || slash == out) return -1;
  *slash = 0;
  return 0;
}

void box64dl_core_init_guest(void) {
  box64context_t* ctx = my_context;
  if (!ctx) return;
  /* No guest main program: libbox64dl runs shared objects / .node inside the
   * host, so the host is the program the guest sees. */
  set_host_program(ctx);
  ctx->envc = 0;
  ctx->envv = (char**)box_calloc(1, sizeof(char*));
  /* No entry point to inherit: guest exec lands on exit_bridge. */
  ctx->ep = ctx->exit_bridge;

  setup_guest_auxv(ctx);

  /* box64path: setter > $LIBBOX64DL_BOX64PATH > packaged box64 > first
   * "box64" in $PATH > NULL (no guest exec of x86_64 children). */
  {
    const char* env = getenv("LIBBOX64DL_BOX64PATH");
    ctx->box64path = s_box64path ? s_box64path
                     : (env && *env) ? box_strdup(env)
                                     : probe_packaged_box64();
    if (!ctx->box64path) ctx->box64path = probe_in_path("box64");
  }

  /* orig_argv: a one-slot writable argv. my_prctl(PR_SET_NAME) strcpy's
   * "*.exe" thread names into orig_argv[0] at any later moment, so this must
   * never be stack storage. Reset in case of a retry. */
  s_orig_argv0[0] = 0;
  ctx->orig_argc = 1;
  ctx->orig_argv = s_orig_argv;

  /* Guest DT_NEEDED like libstdc++.so.6 needs box64_ld_lib populated. */
  LoadLDPath(ctx);
  /* Payload dirs anchored at the host executable. Must run AFTER LoadLDPath
   * (its ParseList wipes entries). LD_LIBRARY_PATH is re-prepended the same way
   * LoadLDPath does, so the environment stays in charge. */
  {
    char dir[PATH_MAX], sub[PATH_MAX];
    static const char* const rel[] = {"../lib64", "../lib", "lib64", "lib"};
    size_t i;
    if (exe_dir(dir, sizeof dir) == 0) {
      for (i = 0; i < sizeof rel / sizeof *rel; ++i)
        if (snprintf(sub, sizeof sub, "%s/%s", dir, rel[i]) < (int)sizeof sub)
          PrependPath(sub, &ctx->box64_ld_lib, 1);
      PrependPath(dir, &ctx->box64_ld_lib, 1);
    }
  }
  if (getenv("LD_LIBRARY_PATH"))
    PrependList(&ctx->box64_ld_lib, getenv("LD_LIBRARY_PATH"), 1);
  AddPath(".", &ctx->box64_ld_lib, 1);

  /* Host libgcc_s is LoongArch; x86_64 libstdc++ needs guest quad-math /
   * unwind symbols. */
  AddPath("libgcc_s.so.1", &ctx->box64_emulated_libs, 0);
  AddPath("libstdc++.so.6", &ctx->box64_emulated_libs, 0);
  AddPath("libunwind.so.8", &ctx->box64_emulated_libs, 0);
  if (BOX64ENV(emulated_libs))
    ParseList(BOX64ENV(emulated_libs), &ctx->box64_emulated_libs, 0);
}

void box64dl_core_load_protection(void) {
  if (my_context) loadProtectionFromMap();
}

void box64dl_core_init_stack(void) {
  if (my_context && my_context->stacksz == 0)
    my_context->stacksz = 8 * 1024 * 1024;
}

/* Public extension (include/libbox64_ext.h): name the box64 binary used for
 * guest-exec children. Defined here because init_guest owns the immortal slot;
 * the old copies are intentionally kept for the process lifetime (the live
 * context may still point at them). */
BOX64_API int box64_set_box64path(const char* path) {
  char* copy;
  if (!path) return -1;
  copy = box_strdup(path);
  if (!copy) return -1;
  s_box64path = copy;
  if (my_context) my_context->box64path = copy; /* live update post-init */
  return 0;
}

/* ------------------------------------------------------------------ */
/* per-thread emu                                                      */

int box64dl_core_new_emu(void) {
  size_t stacksz;
  void* p;
  x64emu_t* emu;
  if (!my_context) {
    box64_set_err("box64: context is not ready");
    return -1;
  }
  stacksz = my_context->stacksz;
  if (stacksz == 0) {
    box64_set_err("box64: guest stack size is zero");
    return -1;
  }
  p = InternalMmap(NULL, stacksz, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_GROWSDOWN, -1, 0);
  if (p == MAP_FAILED) {
    box64_set_err("guest stack mmap failed");
    return -1;
  }
  setProtection_stack((uintptr_t)p, stacksz, PROT_READ | PROT_WRITE);
  emu = NewX64Emu(my_context,
                  my_context->ep ? my_context->ep : my_context->exit_bridge,
                  (uintptr_t)p, (int)stacksz, 1);
  if (!emu) {
    box64_set_err("NewX64Emu failed");
    return -1;
  }
  refreshTLSData(emu);
  thread_set_emu(emu);
  return 0;
}

void* box64dl_core_peek_et(void) { return (void*)thread_get_et(); }
void* box64dl_core_get_emu(void) { return (void*)thread_get_emu(); }
int box64dl_core_context_live(void) { return my_context ? 1 : 0; }
void* box64dl_core_context_ptr(void) { return (void*)my_context; }

/* ------------------------------------------------------------------ */
/* teardown                                                            */

int box64dl_core_end(void) {
  if (!my_context) return 1; /* already gone: nothing to free, report success */
  endBox64();
  /* endBox64() frees the context through FreeBox64Context(&my_context), which
   * sets my_context = NULL. It can refuse instead (emu workers still alive) and
   * return early, leaving the context allocated. Report which happened so Rust
   * never frees a context box64 still owns. */
  return my_context ? 0 : 1;
}

void box64dl_core_free_context(void) {
  if (my_context) FreeBox64Context(&my_context);
}
