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
#include "libbox64_ext.h"

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
/* threads.c (patch 0003): this thread's own emu, or NULL — never fabricates. */
x64emu_t* thread_peek_emu(void);

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

/* set_err with a formatted message, for the diagnostics that carry a value
 * (the offending stack top / GS base). The string is copied into the Rust
 * facade's thread-local slot by box64_set_err, so a stack buffer is fine. */
static void set_err_fmt(const char* fmt, ...) {
  char buf[256];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  box64_set_err(buf);
}

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
  /* The return value is the guest's RAX, so 0 is a legitimate result; an
   * engine-level refusal ALSO returns 0. To keep the two distinguishable every
   * refusal below records the thread-local error slot, and a call that really
   * ran clears it (set_err(NULL) at the end). A caller that treats 0 as a
   * meaningful value (e.g. an NTSTATUS success) must read box64_dlerror() --
   * immediately, it is read-and-consume -- to tell "returned 0" from "never
   * ran". */
  if (!box64_is_ready()) {
    set_err("box64_call_regs: not initialized");
    return 0;
  }
  if (!sym || !gregs || !fregs) {
    set_err("box64_call_regs: NULL sym/gregs/fregs");
    return 0;
  }
  lock();
  if (box64_ensure_emu() != 0) {
    unlock();
    return 0; /* box64_ensure_emu() already recorded the reason */
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
  /* The engine ran the call: clear the slot so a 0 return is unambiguously the
   * guest's own RAX and not a refusal. (An error raised by a nested call
   * during DynaCall belongs to that nested caller, which already read it.) */
  set_err(NULL);
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
  /* Same 0-is-ambiguous contract as box64_call_regs(): refusals record the
   * error slot, a call that ran clears it. */
  if (!box64_is_ready()) {
    set_err("box64_call_marshaled: not initialized");
    return 0;
  }
  if (!sym || !gregs || !fregs) {
    set_err("box64_call_marshaled: NULL sym/gregs/fregs");
    return 0;
  }
  lock();
  if (box64_ensure_emu() != 0) {
    unlock();
    return 0; /* box64_ensure_emu() already recorded the reason */
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
  /* Ran: clear so a 0 return cannot be mistaken for an engine refusal. */
  set_err(NULL);
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

/* Publish a host-made mapping at a guest address (see libbox64.h). The image
 * winedl builds in memory is mapped with the raw host mmap/mprotect, so
 * box64's own protection map has no entry for it; CheckExec() then treats the
 * first branch into an export stub as an access violation. Declaring the range
 * makes getProtection() report the protection the host mapping is supposed to
 * carry.
 *
 * The contract is deliberately the one custommem.c's setProtection() has: this
 * records `prot` VERBATIM. It neither probes nor changes the host pages, so it
 * cannot know whether they really carry `prot`; the embedder that made the
 * mapping owns that guarantee and must verify it against /proc/self/maps,
 * because an mprotect can be vetoed without failing. Recording a protection a
 * page does not have moves the failure from a loud mprotect error to a silent
 * guest fault inside CheckExec() (synthetic SIGSEGV, code 0xecec). */
BOX64_API int box64_declare_host_mapping(void* addr, size_t size,
                                         uint32_t prot) {
  if (!addr || !size) {
    set_err("box64_declare_host_mapping: NULL/empty mapping");
    return -1;
  }
  if (!prot) {
    /* prot == 0 does not publish a mapping: custommem's setProtection() with a
     * zero prot only removes bookkeeping. Refusing keeps "declared" from
     * silently meaning "declared unreachable". */
    set_err("box64_declare_host_mapping: empty protection");
    return -1;
  }
  if (!box64_is_ready()) {
    set_err("box64_declare_host_mapping: not initialized");
    return -1;
  }
  lock();
  setProtection((uintptr_t)addr, size, prot);
  unlock();
  set_err(NULL);
  return 0;
}

/* ------------------------------------------------------------------ *
 * Leaving guest state on the current thread (libbox64_ext.h contract)
 *
 * Root cause this repairs: a host pthread attached to Wine regains control
 * through the PE bridge's longjmp(), which bypasses DynaCall's register
 * restore (dynarec/dynarec.c: DynaCall saves old_rsp and only writes it back
 * on a normal EmuRun return). The x64emu_t is then left with R_RSP inside the
 * guest's Wine PE stack and segs_offs[_GS] pointing at the Wine TEB — both
 * mappings that Wine's __wine_detach_current_thread() frees. Wine's detach
 * entry refuses to run while the emulated SP is inside them, so the switch
 * must happen *before* the detach call, not after it.
 *
 * The anchor is read from the emu, never guessed:
 *
 *   - emu->init_stack / emu->size_stack are the fields internalX64Setup()
 *     (emu/x64emu.c) records for the emu's stack, and
 *     `(init_stack + size_stack) & ~0xF` is the stack top used to enter guest
 *     code (entrypoint.c aligns RSP to 16 before PushExit());
 *   - for a thread that went through box64_ensure_emu() that stack is the
 *     8 MiB anonymous mapping new_x64emu() (rust/shim.c) took with
 *     InternalMmap() and registered with setProtection_stack();
 *   - emu->stack2free is that same owned mapping's base and is munmap()ed
 *     only by FreeX64Emu() -> internalFreeX64() on thread teardown, so while
 *     this runs the region is live;
 *   - the protection table (getProtection_fast(), custommem.c) is consulted
 *     as a fail-closed sanity check: if the chosen region is not a live
 *     read/write Box64 mapping, nothing is changed and the caller gets -1
 *     instead of a wild RSP.
 * ------------------------------------------------------------------ */

/* Fills *top with the 16-byte-aligned RSP to resume guest execution from:
 * the top of the Box64-owned emu stack, ABI-aligned exactly like the engine's
 * own entry path (`RSP & ~0xF`, then PushExit() leaves RSP % 16 == 8 at the
 * callee). Returns 0 and leaves *top valid, or -1 with *top untouched. */
static int emu_host_stack_top(x64emu_t* emu, uint64_t* top) {
  uintptr_t base;
  uint64_t size, cand;

  if (!emu || !emu->init_stack || !emu->size_stack) return -1;
  base = (uintptr_t)emu->init_stack;
  size = (uint64_t)emu->size_stack;
  /* The recorded stack must be sane: page-aligned base, room for a frame. */
  if (box64_pagesize && (base & (box64_pagesize - 1))) return -1;
  if (size < 4096) return -1;
  if (base + size < base) return -1; /* overflow */
  cand = (base + size) & ~(uint64_t)0xF;
  if (cand <= base + 64) return -1;
  /* The first 16 bytes a call touches are [cand-16, cand): PushExit() writes
   * cand-8 and the callee's prologue immediately follows. Both must be a
   * live, writable Box64 mapping. */
  if ((getProtection_fast((uintptr_t)(cand - 16)) & 0x3) != 0x3) return -1;
  if ((getProtection_fast((uintptr_t)(cand - 8)) & 0x3) != 0x3) return -1;
  /* stack2free, when set, must be the base of that same owned mapping. */
  if (emu->stack2free && (uintptr_t)emu->stack2free != base) return -1;
  *top = cand;
  return 0;
}

/* Formal name (see libbox64_ext.h). `box64_detach_current_thread_guest` is
 * kept as the spelling used by the first consumers; both are the same call. */
BOX64_API int box64_prepare_detach_current_thread(box64_guest_state_t* saved) {
  x64emu_t* emu;
  uint64_t top;
  uint64_t old_rsp, old_gs;
  uint16_t old_sel;

  if (saved) memset(saved, 0, sizeof(*saved));
  if (!box64_is_ready()) {
    set_err("box64_prepare_detach_current_thread: not initialized");
    return -1;
  }
  /* Never fabricate an emu here: a thread without one has no guest state to
   * leave, so this is a success no-op rather than a 256 KiB surprise. */
  emu = thread_peek_emu();
  if (!emu) {
    set_err(NULL);
    return 0;
  }
  if (emu_host_stack_top(emu, &top) != 0) {
    set_err("box64_prepare_detach_current_thread: emu has no usable, "
            "writable Box64 stack to resume on");
    return -1;
  }

  old_rsp = R_RSP;
  old_gs = emu->segs_offs[_GS];
  old_sel = emu->segs[_GS];
  if (saved) {
    saved->rsp = old_rsp;
    saved->gs_base = old_gs;
    saved->gs_sel = old_sel;
  }

  /* 1. leave every Wine-owned mapping: RSP moves to the Box64 emu stack. */
  R_RSP = top;
  /* 2. stop looking like a guest to box64dl_signal_route(): a zero GS base is
   *    exactly what a fresh host emu has. FS (wine's unix-side glibc TLS) is
   *    deliberately untouched. */
  emu->segs_offs[_GS] = 0;
  emu->segs[_GS] = 0;
  /* Nothing is allocated and nothing is freed: the former guest stack belongs
   * to Wine's detach entry, and the emu stack stays owned by the emu for the
   * thread's remaining lifetime — no extra stack (1 MiB or otherwise) is
   * leaked on either side. */
  set_err(NULL);
  return 0;
}

BOX64_API int box64_detach_current_thread_guest(box64_guest_state_t* saved) {
  return box64_prepare_detach_current_thread(saved);
}

BOX64_API int box64_detach_current_thread_guest_restore(
    const box64_guest_state_t* saved) {
  x64emu_t* emu;
  if (!saved) {
    set_err("box64_detach_current_thread_guest_restore: NULL snapshot");
    return -1;
  }
  if (!box64_is_ready()) {
    set_err("box64_detach_current_thread_guest_restore: not initialized");
    return -1;
  }
  emu = thread_peek_emu();
  if (!emu) {
    set_err("box64_detach_current_thread_guest_restore: no emu on this thread");
    return -1;
  }
  R_RSP = saved->rsp;
  emu->segs_offs[_GS] = saved->gs_base;
  emu->segs[_GS] = saved->gs_sel;
  set_err(NULL);
  return 0;
}

/* ------------------------------------------------------------------ *
 * Returning to a clean guest stack (libbox64_ext.h contract)
 *
 * Root cause this repairs: a host pthread attached to Wine regains control
 * through the PE bridge's longjmp(), which bypasses DynaCall's register
 * restore. emu RSP is then left on the abandoned PE thread-start frame inside
 * the Wine PE stack, and every later guest call on the thread starts its frame
 * from there. This is the *normal-call* counterpart of the detach switch above:
 * it moves RSP to a clean anchor on that thread's own live PE stack, but it
 * deliberately keeps the guest GS/TEB binding (the thread stays a Wine thread,
 * no detach follow-up is implied).
 *
 * The anchor is computed, never guessed:
 *   - `stack_top' comes from the caller: Wine's TIB.StackBase for this thread
 *     (see box64_current_thread_teb_stack_top()), or the emu stack end for a
 *     caller that has no TEB;
 *   - 0x28 is subtracted so the callee's [rsp+8, rsp+0x28) home area and the
 *     8-byte pushed return slot both stay below the stack top, and RSP keeps
 *     the x86_64 entry parity (RSP % 16 == 8), which is exactly the parity the
 *     post-longjmp RSP already carries;
 *   - nothing is read from or written to the chosen region here beyond the
 *     RSP field, so no protection-map lookup is needed: the region's owner
 *     (Wine or the emu) guarantees it is live at this point.
 * ------------------------------------------------------------------ */

/* x86_64 NT_TIB: ExceptionList +0x00, StackBase +0x08, StackLimit +0x10.
 * `teb' is the guest GS base Wine installs per attached thread; a Wine PE stack
 * top is page-aligned, while a host glibc TCB's +0x08/+0x10 (dtv / self) is
 * not, so the alignment check also rejects a GS base that is not a Wine TEB
 * instead of deriving a stack out of a random control block. Fills *top and
 * returns 0, or returns -1 with *top untouched. */
static int teb_stack_base(uint64_t teb, uint64_t* top) {
  uint64_t base, limit;
  if (!teb || (teb & 0x7)) return -1;
  base = *(const uint64_t*)(uintptr_t)(teb + 0x08);
  limit = *(const uint64_t*)(uintptr_t)(teb + 0x10);
  if (!base || !limit || base < limit) return -1;
  if (base - limit < 0x1000) return -1;
  if (box64_pagesize && (base & (box64_pagesize - 1))) return -1;
  *top = base;
  return 0;
}

BOX64_API int box64_current_thread_teb_stack_top(uint64_t* top) {
  x64emu_t* emu;
  if (!top) {
    set_err("box64_current_thread_teb_stack_top: NULL top");
    return -1;
  }
  *top = 0;
  if (!box64_is_ready()) {
    set_err("box64_current_thread_teb_stack_top: not initialized");
    return -1;
  }
  /* Never fabricate an emu here: a thread without one has no TEB to report. */
  emu = thread_peek_emu();
  if (!emu) {
    set_err("box64_current_thread_teb_stack_top: no emu on this thread");
    return -1;
  }
  if (teb_stack_base(emu->segs_offs[_GS], top) != 0) {
    set_err_fmt("box64_current_thread_teb_stack_top: no usable Wine TEB stack "
                "(guest GS base %#llx)",
                (unsigned long long)emu->segs_offs[_GS]);
    return -1;
  }
  set_err(NULL);
  return 0;
}

BOX64_API int box64_reset_current_thread_guest_stack(uint64_t stack_top) {
  x64emu_t* emu;
  uint64_t top;
  if (!box64_is_ready()) {
    set_err("box64_reset_current_thread_guest_stack: not initialized");
    return -1;
  }
  /* A thread with no emu is a success no-op: it has no guest stack that could
   * be dirty, and fabricating one here would be a 256 KiB surprise. */
  emu = thread_peek_emu();
  if (!emu) {
    set_err(NULL);
    return 0;
  }
  if (!stack_top) {
    set_err("box64_reset_current_thread_guest_stack: zero stack top");
    return -1;
  }
  top = stack_top & ~(uint64_t)0xF;
  if (top < 0x1000) {
    set_err_fmt("box64_reset_current_thread_guest_stack: stack top %#llx too "
                "low",
                (unsigned long long)stack_top);
    return -1;
  }
  /* Only RSP moves. emu->segs_offs[_GS] / emu->segs[_GS] (the TEB binding) and
   * every other register are kept, because this is a normal-call repair, not a
   * detach: the thread must stay a Wine thread with its TEB intact. */
  R_RSP = top - 0x28;
  set_err(NULL);
  return 0;
}

BOX64_API int box64_reset_current_thread_guest_stack_from_teb(void) {
  x64emu_t* emu;
  uint64_t top;
  if (!box64_is_ready()) {
    set_err("box64_reset_current_thread_guest_stack_from_teb: not initialized");
    return -1;
  }
  /* Same no-emu success no-op as the raw setter: a plain host thread has no
   * Wine TEB and no guest stack, so there is nothing to repair. */
  emu = thread_peek_emu();
  if (!emu) {
    set_err(NULL);
    return 0;
  }
  if (teb_stack_base(emu->segs_offs[_GS], &top) != 0) {
    set_err_fmt("box64_reset_current_thread_guest_stack_from_teb: no usable "
                "Wine TEB stack (guest GS base %#llx)",
                (unsigned long long)emu->segs_offs[_GS]);
    return -1;
  }
  return box64_reset_current_thread_guest_stack(top);
}
