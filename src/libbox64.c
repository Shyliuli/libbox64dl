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
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "box64dl_thunk.h"
#include "core_shim.h"
#include "libbox64_diag.h"
#include "libbox64_ext.h"
#include "libbox64_signal.h"

#include <elf.h>

#include "box64context.h"
#include "elfload_dump.h"
#include "elfs/elfloader_private.h"
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

/* Rust phase leaves (rust/src/engine.rs). Short leaves only: each takes the
 * Rust phase lock, mutates the phase word and returns. They never call into the
 * engine, so no Rust frame or guard is live across the C engine calls below. */
int box64dl_rust_phase_begin_init(void);
int box64dl_rust_phase_commit_init(void* ctx);
void box64dl_rust_phase_fail_init(void);
int box64dl_rust_phase_begin_close(void);
void box64dl_rust_phase_commit_close(void);

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

/* ------------------------------------------------------------------ *
 * Engine lifecycle: C orchestration over the Rust phase owner
 *
 * Rust owns the single phase word (rust/src/phase.rs) and exposes short leaf
 * transitions (box64dl_rust_phase_*). This file calls those leaves and then
 * runs the guest-capable engine work in *C* frames, so a foreign siglongjmp can
 * never skip a live Rust frame or guard. The admission gate is likewise plain C
 * state with explicit admit/release; there is no Rust RAII ticket to skip.
 *
 *   box64_lib_init:  begin_init -> engine_boot -> commit_init | fail_init
 *   box64_lib_fini:  begin_close -> quiesce(endBox64) -> commit_close
 *   box64_ensure_emu: admit -> (create emu) -> release
 * ------------------------------------------------------------------ */

static pthread_mutex_t g_admit_mutex = PTHREAD_MUTEX_INITIALIZER;
static int g_admit_closing = 0;
static unsigned g_admit_inflight = 0;

/* Admit one engine operation. Fails when the phase is not READY or a teardown
 * has begun; the caller must pair success with engine_release(). */
static int engine_admit(void) {
  int ok;
  if (!box64_is_ready()) return -1;
  pthread_mutex_lock(&g_admit_mutex);
  ok = !g_admit_closing && box64_is_ready();
  if (ok) ++g_admit_inflight;
  pthread_mutex_unlock(&g_admit_mutex);
  return ok ? 0 : -1;
}

/* Explicit finish (never an RAII ticket). Runs on the normal-return path; the
 * engine's own siglongjmp target is inside DynaCall, so a signal-driven resume
 * still reaches this call. A host longjmp that abandons the frame leaves the
 * count raised on purpose: box64_lib_fini() then refuses teardown instead of
 * force-freeing a live context. */
static void engine_release(void) {
  pthread_mutex_lock(&g_admit_mutex);
  if (g_admit_inflight) --g_admit_inflight;
  pthread_mutex_unlock(&g_admit_mutex);
}

/* Close the gate and report whether calls are still in flight. */
static int engine_quiesce(void) {
  int busy;
  pthread_mutex_lock(&g_admit_mutex);
  g_admit_closing = 1;
  busy = g_admit_inflight != 0;
  pthread_mutex_unlock(&g_admit_mutex);
  return busy ? -1 : 0;
}

/* One-shot boot. Runs while the phase is INITIALIZING and with no Rust frame
 * above it. Any failure after the context exists releases it exactly once. */
static int engine_boot(void) {
  box64dl_core_boot_env();

  if (box64dl_core_signal_policy_publish() != 0) {
    const char* why = box64dl_core_signal_policy_refusal();
    set_err(why ? why : "box64: signal policy refused");
    return -1;
  }
  if (box64dl_core_context_new() != 0) {
    set_err("box64: NewBox64Context failed");
    return -1;
  }
  if (box64dl_core_rebind((const void*)box64_lib_init) != 0) {
    set_err("box64: rebind_box64lib failed");
    box64dl_core_free_context();
    return -1;
  }
  box64dl_core_init_guest();
  box64dl_core_load_protection();
  box64dl_core_init_stack();
  if (box64dl_core_new_emu() != 0) {
    /* the shim recorded the reason in the error slot */
    box64dl_core_free_context();
    return -1;
  }
  return 0;
}

BOX64_API int box64_lib_init(void) {
  int st = box64dl_rust_phase_begin_init();
  if (st == 1) return 0; /* already READY: idempotent */
  if (st != 0) {
    switch (st) {
      case -2:
        set_err("box64: engine initialization already in progress");
        break;
      case -3:
        set_err("box64: engine initialization already failed on this process; "
                "restart the process");
        break;
      case -4:
        set_err("box64: engine is in final shutdown; re-initialization is not "
                "supported, restart the process");
        break;
      default:
        set_err("box64: engine initialization refused");
        break;
    }
    return -1;
  }
  if (engine_boot() != 0) {
    box64dl_rust_phase_fail_init();
    return -1;
  }
  if (box64dl_rust_phase_commit_init(box64dl_core_context_ptr()) != 0) {
    set_err("box64: context handle missing after a successful boot");
    return -1;
  }
  return 0;
}

BOX64_API void box64_lib_fini(void) {
  int busy, freed;
  if (box64dl_rust_phase_begin_close() != 0) return; /* not READY: no-op */
  busy = engine_quiesce();
  if (busy) {
    /* Guest work is still in flight. Publish the terminal state and leave the
     * context allocated: never free it underneath a running call. */
    box64dl_rust_phase_commit_close();
    set_err("box64_lib_fini: refused: guest calls are still in flight; the "
            "engine is FINAL and unusable, exit the process without reusing it");
    return;
  }
  freed = box64dl_core_end(); /* guest fini/destructors; C frame only */
  box64dl_rust_phase_commit_close();
  if (!freed)
    set_err("box64_lib_fini: endBox64() did not free the context (emu workers "
            "still alive); the engine is unusable, exit the process without "
            "reusing it");
}

/* A per-thread emu with the full 8 MiB stack (thread_get_emu's 256 KiB fallback
 * is too small for recursive guests). Admission-bracketed and phase-checked;
 * the allocation happens in C, not under a Rust lock. */
BOX64_API int box64_ensure_emu(void) {
  x64emu_t* emu;
  if (engine_admit() != 0) {
    set_err("box64: context is not ready");
    return -1;
  }
  emu = thread_peek_emu();
  if (!emu) {
    if (box64dl_core_new_emu() != 0) {
      engine_release();
      return -1; /* the shim recorded the reason */
    }
    emu = thread_get_emu();
  }
  engine_release();
  if (!emu) {
    set_err("box64: emu missing after creation");
    return -1;
  }
  return 0;
}

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
  if (engine_admit() != 0) {
    set_err("box64_dlopen: engine is not ready");
    return NULL;
  }
  lock();
  if (box64_ensure_emu() != 0) {
    unlock();
    engine_release();
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
  engine_release();
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
  if (engine_admit() != 0) {
    set_err("box64_dlsym: engine is not ready");
    return NULL;
  }
  lock();
  if (box64_ensure_emu() != 0) {
    unlock();
    engine_release();
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
  engine_release();
  return p;
}

BOX64_API int box64_dlclose(void* handle) {
  x64emu_t* emu;
  int rc;
  BOX64_DIAG("[box64] box64_dlclose handle=%p.\n", handle);
  uintptr_t lo = 0, hi = 0;
  if (!box64_is_ready()) return -1;
  if (engine_admit() != 0) {
    set_err("box64_dlclose: engine is not ready");
    return -1;
  }
  lock();
  if (box64_ensure_emu() != 0) {
    unlock();
    engine_release();
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
  engine_release();
  return rc;
}

/* Protection of the mapping holding `addr`, from /proc/self/maps.
 * 1 = found (*prot set), 0 = unmapped, -1 = maps unreadable. */
static int export_maps_prot(uintptr_t addr, int* prot) {
  FILE* f = fopen("/proc/self/maps", "r");
  char line[512];
  if (!f) return -1;
  while (fgets(line, sizeof(line), f)) {
    unsigned long lo = 0, hi = 0;
    char perms[8];
    if (sscanf(line, "%lx-%lx %7s", &lo, &hi, perms) < 3) continue;
    if (addr < lo || addr >= hi) continue;
    fclose(f);
    *prot = 0;
    if (perms[0] == 'r') *prot |= PROT_READ;
    if (perms[1] == 'w') *prot |= PROT_WRITE;
    if (perms[2] == 'x') *prot |= PROT_EXEC;
    return 1;
  }
  fclose(f);
  return 0;
}

/* Release-store a host address into a guest GOT slot. A RELRO page is opened
 * for the write and restored when its original protection was readable. */
static int export_slot_store(uintptr_t slot, uintptr_t value) {
  long ps = sysconf(_SC_PAGESIZE);
  uintptr_t page;
  int prot = 0, found, opened = 0;

  if (ps <= 0) ps = 4096;
  if (slot % sizeof(uintptr_t)) {
    set_err("box64_export_host: GOT slot is not pointer-aligned");
    return -1;
  }
  page = slot & ~((uintptr_t)ps - 1);
  found = export_maps_prot(slot, &prot);
  if (found == 0) {
    set_err("box64_export_host: GOT slot is not mapped");
    return -1;
  }
  if (found < 0 || !(prot & PROT_WRITE)) {
    int want = (found == 1 ? prot : PROT_READ) | PROT_WRITE;
    if (mprotect((void*)page, (size_t)ps, want) != 0) {
      set_err("box64_export_host: cannot open GOT page");
      return -1;
    }
    opened = 1;
  }
  __atomic_store_n((uintptr_t*)slot, value, __ATOMIC_RELEASE);
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
  if (opened && found == 1)
    mprotect((void*)page, (size_t)ps, prot);
  return 0;
}

BOX64_API int box64_export_host(void* handle, const char* symbol, void* host) {
  library_t* lib;
  elfheader_t* head;
  Elf64_Rela* rela;
  size_t ent, n, i, patched = 0;

  if (!symbol || !symbol[0] || !host) {
    set_err("box64_export_host: null symbol or host");
    return -1;
  }
  if (!box64_is_ready()) {
    set_err("box64_export_host: engine is not ready");
    return -1;
  }
  if (engine_admit() != 0) {
    set_err("box64_export_host: engine is not ready");
    return -1;
  }
  lock();
  lib = dl_get_library(handle);
  if (!lib || lib == (library_t*)-1LL) {
    unlock();
    engine_release();
    set_err("box64_export_host: bad handle");
    return -1;
  }
  head = GetElf(lib);
  if (!head || !head->rela || !head->DynSym._64 || !head->relasz) {
    unlock();
    engine_release();
    set_err("box64_export_host: library has no ELF data relocations");
    return -1;
  }
  ent = head->relaent > 0 ? (size_t)head->relaent : sizeof(Elf64_Rela);
  if (ent < sizeof(Elf64_Rela) || head->relasz / ent == 0) {
    unlock();
    engine_release();
    set_err("box64_export_host: bad DT_RELAENT");
    return -1;
  }
  rela = (Elf64_Rela*)(head->rela + head->delta);
  n = head->relasz / ent;
  for (i = 0; i < n; ++i) {
    unsigned type = ELF64_R_TYPE(rela[i].r_info);
    unsigned symi = ELF64_R_SYM(rela[i].r_info);
    const char* name;
    uintptr_t slot;
    if (type != R_X86_64_GLOB_DAT && type != R_X86_64_64) continue;
    if (head->numDynSym && symi >= head->numDynSym) continue;
    name = SymName64(head, &head->DynSym._64[symi]);
    if (!name || strcmp(name, symbol) != 0) continue;
    slot = (uintptr_t)rela[i].r_offset + (uintptr_t)head->delta;
    if (export_slot_store(slot, (uintptr_t)host) != 0) {
      unlock();
      engine_release();
      return -1;
    }
    patched++;
  }
  unlock();
  engine_release();
  if (!patched) {
    set_err_fmt("box64_export_host: no guest GOT slot for '%s'", symbol);
    return -1;
  }
  set_err(NULL);
  return 0;
}

BOX64_API const char* box64_dlerror(void) { return box64_get_error(); }

/* The seam carries one SSE register as 16 bytes, which must match the engine's
 * sse_regs_t exactly; a mismatch would make box64_call_desc() write a partial
 * register. */
_Static_assert(sizeof(box64_fpr_t) == sizeof(sse_regs_t),
               "box64_fpr_t must match sse_regs_t");

/*
 * Shared prologue for the call entries. The readiness check comes FIRST, before
 * the cached-emu fast path: a thread that still holds an emu from before a
 * teardown must not re-enter the half-torn engine. `box64_is_ready()` is the
 * Rust phase read (CLOSING/FINAL/INITIALIZING => 0), so the fast path is still
 * a lock-free atomic load. A thread WITHOUT an emu takes the ensure slow path.
 */
static x64emu_t* call_entry(const char* who, void* sym,
                            box64_call_status_t* status) {
  x64emu_t* emu;
  if (!sym) {
    set_err_fmt("%s: NULL sym", who);
    if (status) *status = BOX64_CALL_BAD_ARGUMENT;
    return NULL;
  }
  if (!box64_is_ready()) {
    set_err_fmt("%s: not initialized", who);
    if (status) *status = BOX64_CALL_NOT_READY;
    return NULL;
  }
  emu = thread_peek_emu();
  if (emu) return emu;
  lock();
  if (box64_ensure_emu() != 0) {
    unlock();
    if (status) *status = BOX64_CALL_NO_EMU;
    return NULL; /* box64_ensure_emu() already recorded the reason */
  }
  emu = thread_get_emu();
  unlock();
  if (!emu) {
    set_err_fmt("%s: no emulator", who);
    if (status) *status = BOX64_CALL_NO_EMU;
    return NULL;
  }
  return emu;
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

/* The public descriptor converges here.  All public descriptor values carry
 * complete 16-byte SSE registers. */
typedef struct call_request {
  uint32_t flags;
  uint16_t gpr_mask;
  uint16_t fpr_mask;
  uint16_t ret_mask;
  size_t stack_len;
  int set_rax;
  const uint64_t* gprs;
  const box64_fpr_t* fprs;
  const void* stack;
} call_request_t;

static box64_call_status_t call_reject(box64_call_status_t status,
                                       const char* message) {
  set_err(message);
  return status;
}

static box64_call_status_t call_core(void* sym, const call_request_t* req,
                                     box64_call_results_t* out,
                                     const char* who) {
  static const int gpr_pos[6] = {_DI, _SI, _DX, _CX, _R8, _R9};
  x64emu_t* emu;
  box64_call_status_t status = BOX64_CALL_OK;
  size_t i;
  size_t al;
  uint64_t old_rsp = 0;
  uintptr_t old_ip;
  int frame;

  if (!sym)
    return call_reject(BOX64_CALL_BAD_ARGUMENT, "box64 call: NULL sym");
  if (!req)
    return call_reject(BOX64_CALL_BAD_ARGUMENT, "box64 call: NULL request");
  if (req->flags & ~(BOX64_CALL_FLAG_FULL_REGS |
                     BOX64_CALL_FLAG_FORCE_FRAME))
    return call_reject(BOX64_CALL_BAD_LAYOUT, "box64 call: unknown flags");
  if (req->gpr_mask & ~0x3Fu)
    return call_reject(BOX64_CALL_BAD_LAYOUT,
                       "box64 call: gpr_mask outside 0..5");
  if (req->fpr_mask & ~0xFFu)
    return call_reject(BOX64_CALL_BAD_LAYOUT,
                       "box64 call: fpr_mask outside 0..7");
  if (req->ret_mask & ~0x0Fu)
    return call_reject(BOX64_CALL_BAD_LAYOUT,
                       "box64 call: ret_mask outside 0..3");
  if ((req->gpr_mask && !req->gprs) || (req->fpr_mask && !req->fprs) ||
      (req->stack_len && !req->stack))
    return call_reject(BOX64_CALL_BAD_ARGUMENT,
                       "box64 call: mask names a NULL value array");
  /* Admission gate: this is the guest-capable region. The C counter is
   * explicit (no RAII ticket spans the guest); box64_lib_fini() refuses to tear
   * the context down while a call is admitted here. */
  if (engine_admit() != 0)
    return call_reject(BOX64_CALL_NOT_READY, "box64 call: engine is not ready");
  emu = call_entry(who, sym, &status);
  if (!emu) {
    engine_release();
    return status;
  }

  /* Descriptor masks are physical positions: sparse masks never compress the
   * source arrays. Every public descriptor carries complete 16-byte SSE
   * registers, so this constant-width copy is inlined as two register moves
   * instead of a per-register PLT call through a runtime-sized memcpy. */
  for (i = 0; i < 6; ++i)
    if (req->gpr_mask & (uint16_t)(1u << i))
      emu->regs[gpr_pos[i]].q[0] = req->gprs[i];
  for (i = 0; i < 8; ++i)
    if (req->fpr_mask & (uint16_t)(1u << i))
      memcpy(&emu->xmm[i], &req->fprs[i], sizeof(box64_fpr_t));

  /* SysV64 varargs AL: the highest named vector position plus one is the
   * conservative value for sparse physical masks. */
  if (req->set_rax)
    R_RAX = req->fpr_mask
                ? (uint64_t)(32 - __builtin_clz((unsigned)req->fpr_mask))
                : 0;

  /* A stack blob always has an anchored frame. FORCE_FRAME additionally keeps
   * that frame for zero-sized blobs, matching old box64_call_desc(). */
  al = req->stack_len
           ? (((size_t)req->stack_len + 15u) & ~(size_t)15u)
           : 0;
  frame = al != 0 || (req->flags & BOX64_CALL_FLAG_FORCE_FRAME) != 0;
  if (frame) {
    Push64(emu, R_RBP);
    R_RBP = R_RSP;
    R_RSP &= ~(uint64_t)0xF;
    if (al) {
      R_RSP -= al;
      memcpy((void*)(uintptr_t)R_RSP, req->stack, req->stack_len);
      if (al > req->stack_len)
        memset((void*)(uintptr_t)(R_RSP + req->stack_len), 0,
               al - req->stack_len);
    }
  } else {
    old_rsp = call_align(emu);
  }

  old_ip = R_RIP;
  if (req->flags & BOX64_CALL_FLAG_FULL_REGS)
    DynaCall(emu, (uintptr_t)sym, 0);
  else
    DynaCall_abi(emu, (uintptr_t)sym, 0);

  if (frame) {
    if (old_ip == R_RIP) {
      R_RSP = R_RBP;
      R_RBP = Pop64(emu);
    }
  } else {
    call_unwind(emu, old_rsp, old_ip);
  }

  /* Explicit status makes a guest RAX of zero unambiguous. Clear the legacy
   * diagnostic slot for callers that still query box64_dlerror(). A successful
   * DynaCall return is the only path here, so the status is unconditionally OK:
   * there is no post-call refusal to re-check. engine_release() is the explicit
   * finish for the admission above; the engine's own siglongjmp target is
   * inside DynaCall, so it runs on a signal-driven resume too. */
  engine_release();
  set_err(NULL);
  if (out) {
    if (req->ret_mask & BOX64_CALL_RET_RAX) out->rax = R_RAX;
    if (req->ret_mask & BOX64_CALL_RET_RDX) out->rdx = R_RDX;
    if (req->ret_mask & BOX64_CALL_RET_XMM0)
      memcpy(&out->xmm0, &emu->xmm[0], sizeof(box64_fpr_t));
    if (req->ret_mask & BOX64_CALL_RET_XMM1)
      memcpy(&out->xmm1, &emu->xmm[1], sizeof(box64_fpr_t));
  }
  return BOX64_CALL_OK;
}

BOX64_API box64_call_status_t box64_call_desc(
    void* sym, const box64_call_desc_t* desc, box64_call_results_t* out) {
  const char* why = NULL;
  box64_call_status_t status;
  if (!sym)
    return call_reject(BOX64_CALL_BAD_ARGUMENT, "box64_call_desc: NULL sym");
  /* Flags, reserved words, mask bounds and pointer presence come from the one
   * inline rule in libbox64_abi.h, shared with winedl_call_desc(), so the two
   * entries cannot drift into different check sets. */
  status = box64_call_desc_validate(desc, &why);
  if (status != BOX64_CALL_OK) {
    set_err_fmt("box64_call_desc: %s", why ? why : "invalid descriptor");
    return status;
  }
  const call_request_t req = {
      desc->flags,
      desc->gpr_mask,
      desc->fpr_mask,
      desc->ret_mask,
      desc->stack_len,
      1,
      (const uint64_t*)(uintptr_t)desc->gprs,
      (const box64_fpr_t*)(uintptr_t)desc->fprs,
      (const void*)(uintptr_t)desc->stack,
  };
  return call_core(sym, &req, out, "box64_call_desc");
}

BOX64_API box64_call_status_t box64_call_simple(
    void* sym, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
    uint64_t a4, uint64_t a5, uint64_t* rax_out) {
  const uint64_t gprs[6] = {a0, a1, a2, a3, a4, a5};
  const call_request_t req = {
      0, 0x3F, 0, BOX64_CALL_RET_RAX, 0, 1, gprs, NULL, NULL};
  box64_call_results_t result;
  box64_call_status_t status =
      call_core(sym, &req, &result, "box64_call_simple");
  if (status == BOX64_CALL_OK && rax_out) *rax_out = result.rax;
  return status;
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

/* Guest RSP as a bridge callback was entered (include/libbox64_ext.h). */
BOX64_API uint64_t box64_bridge_sp(void* emuv) {
  x64emu_t* emu = (x64emu_t*)emuv;
  if (!emu) return 0;
  return R_RSP;
}

/* Bridge with the default six-integer-register wrapper. */
BOX64_API void* box64_callback(void* host_fn) {
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
  if (engine_admit() != 0) {
    set_err("box64_callback_with: engine is not ready");
    return NULL;
  }
  lock();
  /* Check-first: AddBridge alone allocates a fresh brick per call, so
   * re-registering the same host function would leak bridge slots. */
  br = AddCheckBridge(my_context->system, (wrapper_t)bridge_fn, host_fn, 0,
                      NULL);
  unlock();
  engine_release();
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
  if (engine_admit() != 0) {
    set_err("box64_declare_host_mapping: engine is not ready");
    return -1;
  }
  lock();
  setProtection((uintptr_t)addr, size, prot);
  unlock();
  engine_release();
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
