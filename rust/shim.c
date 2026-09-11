#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "libbox64_ext.h"

#include "auxval.h"
#include "box64context.h"
#include "custommem.h"
#include "debug.h"
#include "env.h"
#include "hostext.h"
#include "os.h"
#include "pathcoll.h"
#include "regs.h"
#include "sysinfo.h"
#include "x64emu.h"
#include "x64tls.h"

#include "emu/x64emu_private.h"

// 由 Rust 侧提供（rust/src/lib.rs 的 box64_set_err）：
// 只锁 err（叶子锁），在持 ctx 锁时调用安全；msg 为 NULL 时清空错误。
void box64_set_err(const char* msg);

void LoadLDPath(box64context_t* context);

extern box64env_t box64env;

void setupTraceInit(void);
void setupTrace(void);

void init_dynarec_host_check(void) {
#ifdef DYNAREC
  if (!DetectHostCpuFeatures())
    SET_BOX64ENV(dynarec, 0);
#endif
}

void *Box64Context_new(void) {
  // 我们的库是用来跑动态链接库的
  // 所以argc=1
  my_context = NewBox64Context(1);
  if (!my_context) {
    return NULL;
  }
  return my_context;
}
/// 用本 so 内任意符号地址定位模块；返回值: 0成功, -1失败
int rebind_box64lib(box64context_t *my_context, const void *box64_fn) {
  int ret = -1;
  Dl_info dli;
  // box64_fn 必须是函数/符号地址，不能取参数自身的栈地址
  int ok = dladdr(box64_fn, &dli);
  if (ok && dli.dli_fname != NULL) {
    //打开box64所在so
    void *self = dlopen(dli.dli_fname, RTLD_NOW | RTLD_GLOBAL);
    // PIE 主程序无法 dlopen 自身路径（glibc 拒绝），退回宿主进程本身。
    // 对可执行文件消费者这正好是对的：GOM my_* 符号就在 -rdynamic 的主程序里。
    // 与旧 C 实现一致：rebind 失败不致命，保留 NewBox64Context 的默认值。
    if (self == NULL) {
      self = dlopen(NULL, RTLD_NOW | RTLD_GLOBAL);
    }
    if (self != NULL) {
        //有效重新绑定
        my_context->box64lib = self;
        ret = 0;
    }
  }
  return ret;
}

/// Embedded-context guest environment.
///
/// Standalone box64 runs a program of its own, and preparing that run
/// initializes three context fields the embedded path (a library that only
/// loads/calls code) never touches — they stay NULL and any guest that
/// reaches for them crashes or silently misbehaves:
///
///   auxval_start  SetupInitialStack (tools/box64stack.c) synthesizes the
///                 guest auxv on the initial stack; without it every guest
///                 getauxval() walks a NULL pointer (wine's unix side asks
///                 for AT_HWCAP2 / AT_PHDR / ... during boot).
///   box64path     standalone resolves it from its own argv[0]; the exec
///                 wrappers re-exec x86_64 children (my_execve,
///                 my_posix_spawn, wrappedlibc.c) THROUGH this path.
///   orig_argv     my_prctl(PR_SET_NAME) strcpy's "*.exe" thread names
///                 into orig_argv[0] (wrappedlibc.c) — wine names its PE
///                 threads exactly that way.
///
/// init_guest fills all three from immortal host memory (the context can
/// be read/written by the engine at any later moment, so nothing here may
/// live on a caller's stack).

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

/* Static storage: the embedded context is a process-wide singleton, and
 * statics survive a fini/re-init cycle without dangling pointers. */
static uintptr_t s_auxv[32];       /* tag/value pairs, AT_NULL-terminated */
static char s_platform[] = "x86_64";
static unsigned char s_at_random[16];
static char s_orig_argv0[256];     /* my_prctl's strcpy target */
static char* s_orig_argv[2] = {s_orig_argv0, NULL};
static char* s_box64path;          /* box64_set_box64path's immortal copy */

/* Mirror of SetupInitialStack's auxv entries, minus the ones that describe
 * a program being run (AT_EXECFD/PHDR/PHENT/PHNUM/ENTRY — the embedded
 * context has no main ELF; wine's get_image_address() explicitly tolerates
 * a zero AT_PHDR). UID/EUID/GID/EGID/CLKTCK/SECURE/RANDOM are taken from
 * the HOST auxv exactly like standalone does. */
static void setup_guest_auxv(box64context_t* my_context) {
  /* HWCAP: same CPUID 1.EDX mirror box64 advertises (box64stack.c) */
  const unsigned long hwcap = 1UL << 0   /* fpu */
                              | 1UL << 4   /* rdtsc */
                              | 1UL << 8   /* cmpxchg8 */
                              | 1UL << 11  /* sep */
                              | 1UL << 15  /* cmov */
                              | 1UL << 19  /* clflush */
                              | 1UL << 23  /* mmx */
                              | 1UL << 24  /* fxsr */
                              | 1UL << 25  /* SSE */
                              | 1UL << 26  /* SSE2 */
                              | 1UL << 28  /* htt */
                              | 1UL << 30; /* ia64 */
  uintptr_t p_random = (uintptr_t)real_getauxval(SHIM_AT_RANDOM);
  unsigned long clktck = real_getauxval(SHIM_AT_CLKTCK);
  int n = 0, i;
  if (!p_random) {
    /* exotic host without AT_RANDOM: make our own bytes (standalone falls
     * back to random() the same way) */
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
  AUX(SHIM_AT_HWCAP2,
      (uintptr_t)(1UL << 1) /* FSGSBASE, the bit box64 itself advertises */);
  AUX(SHIM_AT_EXECFN, (uintptr_t)my_context->argv[0]);
#undef AUX
  for (i = n; i < (int)(sizeof(s_auxv) / sizeof(s_auxv[0])); ++i)
    s_auxv[i] = 0;
  my_context->auxval_start = s_auxv;
}

/* First executable `name` found while walking $PATH, as an immortal copy;
 * NULL when not found. */
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

BOX64_API int box64_set_box64path(const char* path) {
  char* copy;
  if (!path) return -1;
  copy = box_strdup(path);
  if (!copy) return -1;
  /* the old copy is intentionally kept: the live context may still point
   * at it, and a process sets this at most a handful of times */
  s_box64path = copy;
  if (my_context) my_context->box64path = copy; /* live update post-init */
  return 0;
}

/// 宿主可执行文件所在目录（符号链接已解析）；失败返回 -1。
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

void init_guest(box64context_t *my_context) {
  //基本是占位的，因为libbox64dl是用来跑动态链接库/.node的
  my_context->argv[0] = box_strdup("");
  my_context->fullpath = NULL;
  my_context->envc = 0;
  my_context->envv = (char**)box_calloc(1, sizeof(char*));

  //设置入口点，由于这里是libbox64dl
  //lib哪里来的入口点呢？所以这里设置为exit_bridge
  my_context->ep = my_context->exit_bridge;

  // Guest environment the standalone path gets "for free" from running a
  // program (see the block comment above). All storage is immortal.
  setup_guest_auxv(my_context);

  // box64path: setter > $LIBBOX64DL_BOX64PATH > first "box64" in $PATH >
  // NULL (previous behaviour — no guest exec support, nothing regresses).
  {
    const char* env = getenv("LIBBOX64DL_BOX64PATH");
    my_context->box64path = s_box64path ? s_box64path
                          : (env && *env) ? box_strdup(env)
                          : probe_in_path("box64");
  }

  // orig_argv: a one-slot, writable argv. my_prctl(PR_SET_NAME) strcpy's
  // "*.exe" thread names into orig_argv[0] at any later moment, so this
  // must never be stack storage. Reset to "" in case of a re-init.
  s_orig_argv0[0] = 0;
  my_context->orig_argc = 1;
  my_context->orig_argv = s_orig_argv;

  // Embedded init used to skip this. Without it, box64_ld_lib is empty and
  // guest DT_NEEDED like libstdc++.so.6 cannot be found (no wrapper → must
  // emulate the x86_64 .so from BOX64_LD_LIBRARY_PATH / box64-x86_64 paths).
  LoadLDPath(my_context);
  // Payload dirs anchored at the host executable: "." follows the process
  // cwd, so ./build/app launched from the tree root would search the wrong
  // directory. Must go AFTER LoadLDPath — its ParseList wipes any entries
  // already in the collection — and LD_LIBRARY_PATH is re-prepended the
  // same way LoadLDPath does, so the final order keeps the environment in
  // charge: LD_LIBRARY_PATH > exe dirs > BOX64_LD_LIBRARY_PATH/defaults.
  {
    char dir[PATH_MAX], sub[PATH_MAX];
    static const char* const rel[] = {"../lib64", "../lib", "lib64", "lib"};
    size_t i;
    if (exe_dir(dir, sizeof dir) == 0) {
      for (i = 0; i < sizeof rel / sizeof *rel; ++i)
        if (snprintf(sub, sizeof sub, "%s/%s", dir, rel[i]) < (int)sizeof sub)
          PrependPath(sub, &my_context->box64_ld_lib, 1);
      PrependPath(dir, &my_context->box64_ld_lib, 1);
    }
  }
  if (getenv("LD_LIBRARY_PATH"))
    PrependList(&my_context->box64_ld_lib, getenv("LD_LIBRARY_PATH"), 1);
  AddPath(".", &my_context->box64_ld_lib, 1);

  // Host libgcc_s is LoongArch; x86_64 libstdc++ needs guest quad-math /
  // unwind symbols (__getf2, _Unwind_Resume_or_Rethrow, ...).
  AddPath("libgcc_s.so.1", &my_context->box64_emulated_libs, 0);
  AddPath("libstdc++.so.6", &my_context->box64_emulated_libs, 0);
  AddPath("libunwind.so.8", &my_context->box64_emulated_libs, 0);
  if (BOX64ENV(emulated_libs))
    ParseList(BOX64ENV(emulated_libs), &my_context->box64_emulated_libs, 0);
}
void init_box64_trace(){
  setupTraceInit();
  setupTrace();
}
void init_sysinfo(){
    InitializeSystemInfo();
}

void init_stack(box64context_t *my_context){
  if(my_context->stacksz==0){
    my_context->stacksz=8*1024*1024;
  }
}

size_t context_stack_size(box64context_t *my_context){
  return my_context->stacksz;
}
// 创建emu，rust侧包装要求所有错误均返回NULL
x64emu_t* new_x64emu(box64context_t *my_context){
  if (my_context->stacksz == 0){
    return NULL;
  }
  size_t stacksz = my_context->stacksz;
  //分配栈空间
  void* p = InternalMmap(NULL, stacksz, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS | MAP_GROWSDOWN, -1, 0);
    if (p == MAP_FAILED) {
       box64_set_err("guest stack mmap failed");
      return NULL;
     }
    //设置栈保护
    setProtection_stack((uintptr_t)p, stacksz, PROT_READ | PROT_WRITE);
    uintptr_t stack = (uintptr_t)p;
    int own_stack = 1;

    //创建emu
    x64emu_t* emu = NewX64Emu(my_context,
                   my_context->ep ? my_context->ep : my_context->exit_bridge,
                   stack, stacksz, own_stack);
  
  // 不需要设置emu，NewX64Emu已经设置好了
  // SetupX64Emu(emu, NULL);

  //给guest分配TLS数据
  refreshTLSData(emu);
  //设置线程绑定的emu变量
  thread_set_emu(emu);
  return emu;
}

/* Stack sibling of the public bridge accessors (box64_bridge_args /
 * box64_bridge_ret in libbox64.c): the guest RSP as the bridge callback
 * was entered. Contract in libbox64_ext.h — [rsp+0] is the guest's return
 * address, stack-passed arguments follow (SysV: from rsp+8; other guest
 * ABIs layered on this engine must translate, e.g. winx64 args at
 * rsp+0x28 after the 32-byte shadow space). */
BOX64_API uint64_t box64_bridge_sp(void* emuv) {
  x64emu_t* emu = (x64emu_t*)emuv;
  if (!emu) return 0;
  return R_RSP;
}