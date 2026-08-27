#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdint.h>
#include <sys/mman.h>

#include "box64context.h"
#include "custommem.h"
#include "debug.h"
#include "env.h"
#include "hostext.h"
#include "os.h"
#include "pathcoll.h"
#include "sysinfo.h"
#include "x64emu.h"
#include "x64tls.h"

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

void init_guest(box64context_t *my_context) {
  //基本是占位的，因为libbox64dl是用来跑动态链接库/.node的
  my_context->box64path = NULL;
  my_context->argv[0] = box_strdup("");
  my_context->fullpath = NULL;
  my_context->envc = 0;
  my_context->envv = (char**)box_calloc(1, sizeof(char*));
  
  //设置入口点，由于这里是libbox64dl
  //lib哪里来的入口点呢？所以这里设置为exit_bridge
  my_context->ep = my_context->exit_bridge;

  // Embedded init used to skip this. Without it, box64_ld_lib is empty and
  // guest DT_NEEDED like libstdc++.so.6 cannot be found (no wrapper → must
  // emulate the x86_64 .so from BOX64_LD_LIBRARY_PATH / box64-x86_64 paths).
  LoadLDPath(my_context);
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