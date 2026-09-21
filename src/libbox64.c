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
#include "libbox64_diag.h"
#include "libbox64_ext.h"
#include "libbox64_signal.h"

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
  BOX64_DIAG("[box64] box64_lib_fini enter.\n");
  lock();
  if (!box64_is_ready()) {
    unlock();
    return;
  }
  endBox64();
  /* endBox64 -> fini_signal_helper() now restores the pre-box64 host actions it
   * saved at init (patches/0006) instead of resetting the four signals to
   * SIG_DFL, so a host handler (V8's wasm trap handler among them) survives
   * fini. It only touches a signal whose disposition is still box64's own. */
  my_context = NULL;
  unlock();
}

BOX64_API int box64_lib_ready(void) { return box64_is_ready(); }

/*
 * Signal policy (include/libbox64.h). The global default is one-shot and must
 * happen before box64_lib_init(): the engine publishes it into a plain int
 * before any guest call can run. A call after publication fails and changes
 * nothing. Per-library overrides are separate (below) and may be changed while
 * the engine is up; see the header for the sequential-config contract.
 */
BOX64_API int box64_signal_policy_set(int policy) {
  if (box64dl_signal_policy_request(policy) != 0) {
    set_err("box64_signal_policy_set: invalid policy or already published");
    return -1;
  }
  return 0;
}

BOX64_API int box64_signal_policy_get(void) {
  return box64dl_signal_policy_mode();
}

/* Resolve an open handle to its loaded image range. Returns 0 and fills
 * lo/size, or -1 with the error slot set. A handle that is not a live box64
 * dlopen handle, or a wrapped library with no ELF image, is refused: neither
 * has a guest address range to key a policy on. */
static int handle_image_range(void* handle, uintptr_t* lo, uint32_t* size) {
  library_t* lib;
  elfheader_t* elf;
  if (!my_context) {
    set_err("box64_signal_policy: not initialized");
    return -1;
  }
  lock();
  lib = dl_get_library(handle);
  if (!lib || lib == (library_t*)-1LL) {
    unlock();
    set_err("box64_signal_policy: invalid or closed handle");
    return -1;
  }
  elf = GetElf(lib);
  if (!elf) {
    unlock();
    set_err("box64_signal_policy: library has no ELF image (wrapped)");
    return -1;
  }
  *lo = (uintptr_t)GetBaseAddress(elf);
  *size = GetBaseSize(elf);
  unlock();
  if (!*lo || !*size) {
    set_err("box64_signal_policy: library image range is empty");
    return -1;
  }
  return 0;
}

/*
 * Per-library signal policy (include/libbox64.h). An explicit policy here
 * takes precedence over an environment rule for the same library, which in
 * turn takes precedence over the global default. The range is registered under
 * the library's image base, so the hot resolver reaches it through its O(1)
 * cache. Constructors have already run by the time an embedder can obtain a
 * handle, so this applies to later host entries only; an environment rule is
 * what covers constructors (bound in the engine's RunElfInit).
 */
BOX64_API int box64_signal_policy_set_library(void* handle, int policy) {
  uintptr_t lo = 0;
  uint32_t size = 0;
  if (policy != BOX64_SIGNAL_POLICY_COMPAT && policy != BOX64_SIGNAL_POLICY_FAST) {
    set_err("box64_signal_policy_set_library: invalid policy");
    return -1;
  }
  if (!box64_is_ready()) {
    set_err("box64_signal_policy_set_library: not initialized");
    return -1;
  }
  if (handle_image_range(handle, &lo, &size) != 0) return -1;
  if (box64dl_signal_lib_range_set(lo, lo + size, policy) != 0) {
    set_err("box64_signal_policy_set_library: policy table full");
    return -1;
  }
  set_err(NULL);
  return 0;
}

/* The effective policy for an open library: an explicit override or a matching
 * environment rule if one applies, otherwise the global default. */
BOX64_API int box64_signal_policy_get_library(void* handle) {
  uintptr_t lo = 0;
  uint32_t size = 0;
  if (!box64_is_ready()) {
    set_err("box64_signal_policy_get_library: not initialized");
    return -1;
  }
  if (handle_image_range(handle, &lo, &size) != 0) return -1;
  return box64dl_signal_policy_for_addr(lo);
}

/*
 * Foreign-loader image policy (include/libbox64_ext.h). The Wine/PE path owns
 * its modules, so it registers their (base, size) directly; the same range
 * registry serves ELF and PE addresses. Cold config only: no guest entry may
 * run on any thread while these execute.
 */
BOX64_API int box64_signal_policy_set_range(uintptr_t lo, size_t size,
                                            int policy) {
  if (!box64_is_ready()) {
    set_err("box64_signal_policy_set_range: not initialized");
    return -1;
  }
  if (!lo || !size || lo > (uintptr_t)-1 - (uintptr_t)size) {
    set_err("box64_signal_policy_set_range: invalid/overflowing range");
    return -1;
  }
  if (policy != BOX64_SIGNAL_POLICY_COMPAT && policy != BOX64_SIGNAL_POLICY_FAST) {
    set_err("box64_signal_policy_set_range: invalid policy");
    return -1;
  }
  if (box64dl_signal_lib_range_set(lo, lo + (uintptr_t)size, policy) != 0) {
    set_err("box64_signal_policy_set_range: policy table full");
    return -1;
  }
  set_err(NULL);
  return 0;
}

BOX64_API int box64_signal_policy_get_address(uintptr_t addr) {
  /* Always a mode: a foreign loader asks for the effective policy of a module
   * address. With no registered range/rule it is the global default. */
  return box64dl_signal_policy_for_addr(addr);
}

BOX64_API void box64_signal_policy_clear_range(uintptr_t lo) {
  if (!lo) return;
  box64dl_signal_lib_range_clear(lo);
}

BOX64_API int box64_signal_policy_rule_for_name(const char* name) {
  if (!name || !*name) return -1;
  return box64dl_signal_lib_rule_lookup(name);
}

/*
 * Remove a call frame abandoned by a host non-local jump (include/libbox64_ext.h).
 * Restores the exact no-frame state EmuRun's epilogue would have written, so the
 * next call arms a fresh frame. No emu -> no-op (never fabricates one); the
 * error slot and jmpbuf_ready are deliberately untouched.
 */
BOX64_API void box64_call_frame_reset(void) {
  x64emu_t* emu = thread_peek_emu();
  if (!emu) return;
  emu->jmpbuf = NULL;
  emu->active_jmpbuf_policy = BOX64DL_POLICY_INHERIT;
  emu->requested_call_policy = BOX64DL_POLICY_INHERIT;
}

/* Diagnostics only; the per-call counters are per-thread and non-atomic. */
BOX64_API void box64_signal_stats_enable(int on) {
  box64dl_sig_dbg_on = on ? 1 : 0;
}

BOX64_API void box64_signal_stats(box64_signal_stats_t* out) {
  if (!out) return;
  out->setjmp_saves = box64dl_sig_dbg_saves();
  out->setjmp_skips = box64dl_sig_dbg_skips();
  out->mask_restores = box64dl_sig_dbg_restores();
  out->publishes = box64dl_sig_dbg_publishes();
}

BOX64_API void box64_set_wine_mode(void) {
  extern int box64_wine;
  box64_wine = 1;
}

BOX64_API void* box64_dlopen(const char* path, int flags) {
  x64emu_t* emu;
  void* handle;
  BOX64_DIAG("[box64] box64_dlopen path=%s.\n", path ? path : "(null)");
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
  BOX64_DIAG("[box64] box64_dlsym name=%s.\n", name ? name : "(null)");
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
  BOX64_DIAG("[box64] box64_dlclose handle=%p.\n", handle);
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

  /* dlclose only drops a reference; release trampolines only once the ELF is
   * really gone. The signal-policy range is NOT cleared here: it is cleared at
   * the single real unmap point (FreeElfMemory), so a dependency unloaded by a
   * guest my_dlclose is covered too, not just this top-level handle. */
  if (hi > lo && !FindElfAddress(my_context, lo))
    box64dl_thunk_release_range(lo, hi);

  unlock();
  return rc;
}

BOX64_API const char* box64_dlerror(void) { return box64_get_error(); }

/* The seam carries one SSE register as 16 bytes, which must match the engine's
 * sse_regs_t exactly; a mismatch would make box64_call_plan() write a partial
 * register. */
_Static_assert(sizeof(box64_fpr_t) == sizeof(sse_regs_t),
               "box64_fpr_t must match sse_regs_t");

/*
 * Shared prologue fast path for the fixed-arity call entries (and the reason
 * the old entries no longer repeat the ensure): thread_peek_emu() is a bare
 * pthread_getspecific (patches/0003), so a thread that already has an emu can
 * start the guest call without taking the shared recursive context lock,
 * without re-checking readiness (a live per-thread emu implies a live
 * context) and without a second box64_ensure_emu(). A thread WITHOUT an emu
 * still takes the original slow path unchanged — the per-thread ensure is
 * preserved, not removed.
 */
static x64emu_t* call_entry(const char* who, void* sym) {
  x64emu_t* emu;
  if (!sym) {
    set_err_fmt("%s: NULL sym", who);
    return NULL;
  }
  /* my_context is NULL before init and after box64_lib_fini(), while the Rust
   * facade's ready Option can remain Some across fini; keying the fast path on
   * my_context keeps a thread that still holds an emu from calling into a
   * torn-down engine. It is a plain pointer load, so the hot path stays
   * lock-free; box64_lib_init() populates it before any call can run. This is
   * a SEQUENTIAL guard:
   * box64_lib_fini() must not race calls on other threads, the same lifetime
   * contract box64_call_regs() always had — it is not synchronisation. */
  if (!my_context) {
    set_err_fmt("%s: not initialized", who);
    return NULL;
  }
  emu = thread_peek_emu();
  if (emu) return emu;
  if (!box64_is_ready()) {
    set_err_fmt("%s: not initialized", who);
    return NULL;
  }
  lock();
  if (box64_ensure_emu() != 0) {
    unlock();
    return NULL; /* box64_ensure_emu() already recorded the reason */
  }
  emu = thread_get_emu();
  unlock();
  return emu;
}

/* Shared epilogue: the call ran, so a 0 return is unambiguously the guest's own
 * RAX and not a refusal. (An error raised by a nested call during DynaCall
 * belongs to that nested caller, which already read it.) */
static uint64_t call_finish(x64emu_t* emu) {
  set_err(NULL);
  return R_RAX;
}

/* Align the outgoing RSP to the engine's entry convention: DynaCall's
 * PushExit() subtracts 8, so RSP must be 16-aligned at DynaCall entry for the
 * guest to see the ABI parity rsp % 16 == 8. Returns the caller's RSP for
 * call_unwind(). Needed even on the no-stack path: a re-entrant call made from
 * a bridge callback starts from the outer guest frame's RSP, whose parity is
 * not the caller's. */
static uint64_t call_align(x64emu_t* emu) {
  uint64_t old = R_RSP;
  R_RSP = old & ~(uint64_t)0xF;
  return old;
}

/* Put the caller's RSP back on a normal return. On a guest longjmp DynaCall
 * deliberately leaves the abandoned RSP in place, so it is kept (restoring
 * would clobber the longjmp target's stack). */
static void call_unwind(x64emu_t* emu, uint64_t old_rsp, uintptr_t old_ip) {
  if (old_ip == R_RIP) R_RSP = old_rsp;
}

BOX64_API uint64_t box64_call_regs(void* sym, uint64_t* gregs, uint64_t* fregs) {
  x64emu_t* emu;
  int nn[] = {_DI, _SI, _DX, _CX, _R8, _R9};
  int i;
  uint64_t ret;
  uint64_t old_rsp;
  uintptr_t old_ip;
  BOX64_DIAG("[box64] box64_call_regs sym=%p.\n", sym);
  /* The return value is the guest's RAX, so 0 is a legitimate result; an
   * engine-level refusal ALSO returns 0. To keep the two distinguishable every
   * refusal below records the thread-local error slot, and a call that really
   * ran clears it (set_err(NULL) at the end). A caller that treats 0 as a
   * meaningful value (e.g. an NTSTATUS success) must read box64_dlerror() --
   * immediately, it is read-and-consume -- to tell "returned 0" from "never
   * ran". */
  emu = thread_peek_emu();
  if (!my_context || (!emu && !box64_is_ready())) {
    set_err("box64_call_regs: not initialized");
    return 0;
  }
  if (!sym || !gregs || !fregs) {
    set_err("box64_call_regs: NULL sym/gregs/fregs");
    return 0;
  }
  if (!emu) {
    lock();
    if (box64_ensure_emu() != 0) {
      unlock();
      return 0; /* box64_ensure_emu() already recorded the reason */
    }
    emu = thread_get_emu();
    unlock();
  }

  for (i = 0; i < 6; ++i) emu->regs[nn[i]].q[0] = gregs[i];
  for (i = 0; i < 8; ++i) memcpy(&emu->xmm[i], &fregs[i], sizeof(uint64_t));

  /* emu and its stack are thread-local; guest execution runs unserialized.
   * Align the outgoing RSP (ABI entry parity, robust to a re-entrant caller)
   * and restore the caller's RSP on a normal return. DynaCall selects the
   * per-library signal policy from `sym` centrally (DynaCallCore). */
  old_rsp = call_align(emu);
  old_ip = R_RIP;
  DynaCall(emu, (uintptr_t)sym, 0);
  call_unwind(emu, old_rsp, old_ip);

  ret = R_RAX;
  gregs[0] = R_RAX;
  gregs[1] = R_RDX;
  memcpy(&fregs[0], &emu->xmm[0], sizeof(uint64_t));
  memcpy(&fregs[1], &emu->xmm[1], sizeof(uint64_t));
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
  size_t al;
  BOX64_DIAG("[box64] box64_call_marshaled sym=%p stack_len=%zu.\n", sym,
             stack_len);
  /* Same 0-is-ambiguous contract as box64_call_regs(): refusals record the
   * error slot, a call that ran clears it. */
  emu = thread_peek_emu();
  if (!my_context || (!emu && !box64_is_ready())) {
    set_err("box64_call_marshaled: not initialized");
    return 0;
  }
  if (!sym || !gregs || !fregs) {
    set_err("box64_call_marshaled: NULL sym/gregs/fregs");
    return 0;
  }
  if (!emu) {
    lock();
    if (box64_ensure_emu() != 0) {
      unlock();
      return 0; /* box64_ensure_emu() already recorded the reason */
    }
    emu = thread_get_emu();
    unlock();
  }

  Push64(emu, R_RBP);
  R_RBP = R_RSP;
  /* Entry-alignment fix. The engine's own entry path (entrypoint.c) does
   * `Push64(RBP); RBP=RSP; RSP &= ~0xF; PushExit();` and PushExit subtracts 8,
   * so the guest must be entered with (pre-PushExit RSP) % 16 == 0 to get the
   * ABI parity rsp % 16 == 8. The old code skipped the mask and only happened
   * to work when the caller's RSP was already 8 mod 16; it was 8 bytes off for
   * a 16-aligned emu RSP (measured with a naked-asm probe), and it also left
   * the outgoing blob 8-mod-16 aligned. Masking down here makes the parity
   * independent of the caller and puts the blob on a 16-byte boundary, which
   * is what an aligned SSE access to a stack aggregate requires. */
  R_RSP &= ~(uint64_t)0xF;
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

  /* DynaCall selects the per-library signal policy from `sym` centrally. */
  oldip = R_RIP;
  DynaCall(emu, (uintptr_t)sym, 0);
  /* Normal return: undo the RBP anchor and the outgoing blob so the caller's
   * RSP and RBP are exactly what they were. On a guest longjmp DynaCall
   * deliberately leaves the abandoned RSP in place, so the frame is not
   * unwound — restoring here would clobber the longjmp target's stack. */
  if (oldip == R_RIP) {
    R_RSP = R_RBP;
    R_RBP = Pop64(emu);
  }

  ret = R_RAX;
  gregs[0] = R_RAX;
  gregs[1] = R_RDX;
  memcpy(&fregs[0], &emu->xmm[0], sizeof(uint64_t));
  memcpy(&fregs[1], &emu->xmm[1], sizeof(uint64_t));
  set_err(NULL);
  return ret;
}

/* Fixed-arity integer/pointer entries. Script-generated (real C parameter
 * lists, one direct store per used argument register; no fmt re-parse and no
 * FPR/stack traffic). Regenerate with `python3 tools/gen_call_arity.py`; the
 * generated file is checked in. */
#include "libbox64_call_arity.inc"

/*
 * Compiled-layout entry (see include/libbox64.h). The layout is a static
 * const produced by the stub generator; this code only applies the masks and
 * the byte count, it never parses or classifies a signature. Compared with
 * box64_call_regs()/box64_call_marshaled() it copies exactly the argument
 * registers the layout names, never touches an unnamed SSE register, and skips
 * the outgoing frame entirely when stack_len is 0.
 *
 * Like the fixed-arity entries it enters through DynaCall_abi(), the known-ABI
 * variant of DynaCall(): `sym` must be a compiled function of the convention
 * the plan encodes. The seam can express a Win64 register placement, but no
 * Win64 adapter uses this entry today; the Wine path writes the register file
 * through box64_call_regs()/box64_call_marshaled(), which keep the generic
 * DynaCall() behavior unchanged. In-tree callers (unilib) use the SysV64
 * placement.
 */
BOX64_API uint64_t box64_call_plan(void* sym, const box64_sigplan_t* plan,
                                   const uint64_t* gprs,
                                   const box64_fpr_t* fprs, const void* stack,
                                   box64_call_results_t* out) {
  x64emu_t* emu;
  uint32_t m;
  int i;
  uintptr_t oldip;
  size_t al;
  uint64_t old_rsp = 0;
  uint64_t ret;
  static const int gpr_pos[6] = {_DI, _SI, _DX, _CX, _R8, _R9};

  BOX64_DIAG("[box64] box64_call_plan sym=%p gpr=%#x fpr=%#x stack=%u.\n", sym,
             plan ? plan->gpr_mask : 0, plan ? plan->fpr_mask : 0,
             plan ? plan->stack_len : 0);
  if (!plan) {
    set_err("box64_call_plan: NULL plan");
    return 0;
  }
  /* The masks only have meaning over the engine register file; a bit outside
   * it is a generator bug, refused rather than silently ignored. */
  if (plan->gpr_mask & ~0x3Fu) {
    set_err("box64_call_plan: gpr_mask names a position outside 0..5");
    return 0;
  }
  if (plan->fpr_mask & ~0xFFu) {
    set_err("box64_call_plan: fpr_mask names a position outside 0..7");
    return 0;
  }
  if (plan->ret_mask & ~0x0Fu) {
    set_err("box64_call_plan: ret_mask names a position outside 0..3");
    return 0;
  }
  if ((plan->gpr_mask && !gprs) || (plan->fpr_mask && !fprs) ||
      (plan->stack_len && !stack)) {
    set_err("box64_call_plan: mask names a NULL value array");
    return 0;
  }

  emu = call_entry("box64_call_plan", sym);
  if (!emu) return 0;

  /* Argument register file: only the positions the layout names, in position
   * order. Unnamed positions keep whatever the emu had (ordinary caller-saved
   * semantics for the callee). */
  for (m = plan->gpr_mask, i = 0; m; m >>= 1, ++i)
    if (m & 1u) emu->regs[gpr_pos[i]].q[0] = gprs[i];
  for (m = plan->fpr_mask, i = 0; m; m >>= 1, ++i)
    if (m & 1u) memcpy(&emu->xmm[i], &fprs[i], sizeof(box64_fpr_t));

  /* SysV64 varargs contract on entry: AL = number of vector registers used.
   * For a sparse mask the true count is ambiguous, so use the safe upper bound
   * highest-used + 1 (equal to the count for the contiguous masks a real
   * signature produces, and enough for a callee's register save area when a
   * plan does have holes). A non-varargs callee ignores it, and the return
   * overwrites RAX. __builtin_clz is guarded because it is undefined at 0. */
  R_RAX = plan->fpr_mask
              ? (uint64_t)(32 - __builtin_clz((unsigned)plan->fpr_mask))
              : 0;

  /* Outgoing stack blob, exactly the box64_call_marshaled() shape and only
   * when there is one: any byte count is rounded up to 16, the tail is
   * zeroed, and RBP is pushed only to anchor the restore. stack_len is a
   * uint16_t, so (stack_len + 15) fits size_t with room to spare and the
   * rounded value never overflows (at most 65536). */
  al = plan->stack_len ? (((size_t)plan->stack_len + 15u) & ~(size_t)15u) : 0;
  if (al) {
    Push64(emu, R_RBP);
    R_RBP = R_RSP;
    /* Same entry-alignment fix as box64_call_marshaled (and the engine's own
     * entrypoint.c): mask the frame anchor down to 16 so PushExit(-8) leaves
     * the ABI entry parity and the stack blob starts 16-byte aligned. */
    R_RSP &= ~(uint64_t)0xF;
    R_RSP -= al;
    memcpy((void*)(uintptr_t)R_RSP, stack, plan->stack_len);
    if (al > plan->stack_len)
      memset((void*)(uintptr_t)(R_RSP + plan->stack_len), 0,
             al - plan->stack_len);
  } else {
    /* No outgoing frame: still align RSP for the ABI entry parity, and because
     * a re-entrant call from a bridge callback starts on the outer guest
     * frame's RSP. */
    old_rsp = call_align(emu);
  }

  oldip = R_RIP;
  DynaCall_abi(emu, (uintptr_t)sym, 0);
  /* Same unwind rule as box64_call_marshaled: on a normal return restore the
   * caller's RSP/RBP exactly; on a guest longjmp leave the RSP DynaCall_abi
   * left (it keeps DynaCall()'s longjmp rule verbatim). */
  if (al) {
    if (oldip == R_RIP) {
      R_RSP = R_RBP;
      R_RBP = Pop64(emu);
    }
  } else {
    call_unwind(emu, old_rsp, oldip);
  }

  ret = R_RAX;
  if (out) {
    if (plan->ret_mask & 0x1u) out->rax = R_RAX;
    if (plan->ret_mask & 0x2u) out->rdx = R_RDX;
    if (plan->ret_mask & 0x4u) memcpy(&out->xmm0, &emu->xmm[0], sizeof(box64_fpr_t));
    if (plan->ret_mask & 0x8u) memcpy(&out->xmm1, &emu->xmm[1], sizeof(box64_fpr_t));
  }
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
  BOX64_DIAG("[box64] box64_bridge_args emu=%p.\n", emuv);
  int nn[] = {_DI, _SI, _DX, _CX, _R8, _R9};
  int i;
  if (!emu || !gregs || !fregs) return;
  for (i = 0; i < 6; ++i) gregs[i] = emu->regs[nn[i]].q[0];
  for (i = 0; i < 8; ++i) memcpy(&fregs[i], &emu->xmm[i], sizeof(uint64_t));
}

BOX64_API void box64_bridge_ret(void* emuv, uint64_t rax, uint64_t rdx,
                                const uint64_t* fregs2) {
  x64emu_t* emu = (x64emu_t*)emuv;
  BOX64_DIAG("[box64] box64_bridge_ret rax=0x%llx.\n",
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
  BOX64_DIAG("[box64] box64_callback_with host_fn=%p.\n", host_fn);
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
