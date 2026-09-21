/*
 * libbox64dl: signal-handling policy (see include/libbox64_signal.h).
 *
 * Global default publication, the per-entry resolver, the per-library override
 * registry and the slow-path restore. Deliberately free of any dependency on
 * the emu or the vendored tree: the vendor side reaches it through the
 * declarations in include/libbox64_signal.h and passes values (addresses, the
 * active frame policy) rather than structures. That keeps this file linkable
 * into the static engine, the PIC engine and the shared library unchanged.
 *
 * Hot-path shape:
 *   - no library override registered -> box64dl_signal_policy_for_addr() is a
 *     single relaxed load of g_ranges_n plus a return of the global default;
 *   - overrides registered -> per-thread direct-mapped cache, refilled by a
 *     short linear scan on a miss. No lock, no FindElfAddress, no seqlock.
 *
 * CONFIG CONTRACT: the range table is written only at load/config time and the
 * embedder must not change a policy while guest calls run on another thread.
 * The fields are atomic so a signal handler that re-enters during a load
 * observes no torn value; no coherent multi-field snapshot is promised under
 * concurrent configuration, and the resolver never blocks on a writer.
 */
#define _GNU_SOURCE
#include "libbox64_signal.h"

#include "libbox64.h"

#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ucontext.h>

/* --- published global default (read by the hot path) ---------------------- */

/* FAST is the default. COMPAT is opt-in for an embedder that needs the old
 * "reset the mask to the EmuRun-entry value on resume" semantics. */
static int g_mode = BOX64DL_POLICY_FAST;
static int g_pending = -1;
static int g_published = 0;
static int g_refused = 0;
static int g_publishes = 0;
static const char* g_refusal = "";

int box64dl_sig_dbg_on = 0;
int box64dl_route_trace = 0;

/* --- diagnostics ---------------------------------------------------------- */

/* saves/skips are per-thread and not atomic: a __thread slot has a single
 * writer (the thread itself, from EmuRun) and they are only touched outside a
 * signal handler, so TLS resolution never happens in the handler. They are
 * gated by dbg_on (default off), so the default cost is a predictable branch.
 *
 * restores is incremented from the signal handler, where a __thread access
 * could call __tls_get_addr and take a loader lock on first touch. It is
 * therefore process-wide with a relaxed atomic increment. */
static __thread unsigned long t_saves;
static __thread unsigned long t_skips;
static atomic_ulong g_restores;

void box64dl_sig_dbg_note(int saved) {
  if (saved)
    t_saves++;
  else
    t_skips++;
}

unsigned long box64dl_sig_dbg_saves(void) { return t_saves; }
unsigned long box64dl_sig_dbg_skips(void) { return t_skips; }
unsigned long box64dl_sig_dbg_restores(void) {
  return atomic_load_explicit(&g_restores, memory_order_relaxed);
}
int box64dl_sig_dbg_publishes(void) { return g_publishes; }

/* Defined with the environment-rule parser below. A malformed rule is a
 * configuration error and refuses publication (init), rather than being
 * silently skipped and leaving the library on the FAST default. */
static int rules_valid(void);

/* --- global default selection / publication ------------------------------- */

/* Only the two real policies are accepted. The aliases the old build carried
 * ("safe", "default", "0"/"1", the env-only "none") are gone: they silently
 * changed meaning when the default flipped to FAST, and "none" promised a
 * handler-less mode the engine cannot run without. An unknown value is refused
 * with one message. */
static int policy_from_name(const char* s) {
  if (!s || !*s) return g_mode; /* unset: keep the built-in default */
  if (!strcmp(s, "compat")) return BOX64DL_POLICY_COMPAT;
  if (!strcmp(s, "fast")) return BOX64DL_POLICY_FAST;
  return -1;
}

int box64dl_signal_policy_request(int policy) {
  if (policy != BOX64DL_POLICY_COMPAT && policy != BOX64DL_POLICY_FAST)
    return -1;
  if (g_published) return -1;
  g_pending = policy;
  return 0;
}

int box64dl_signal_policy_publish(void) {
  const char* env;
  int mode;

  if (g_published) return g_refused ? -1 : 0;

  mode = g_pending;
  if (mode < 0) {
    env = getenv("BOX64DL_SIGNAL_POLICY");
    mode = policy_from_name(env);
    if (mode < 0) {
      /* Includes the old "none" spelling. There is no handler-less policy:
       * dynarec protectDB/auto-SMC and guest fatal-signal/SEH emulation depend
       * on the SIGSEGV/SIGBUS/SIGILL/SIGABRT handlers, so an embedder asking
       * for one is refused rather than run unprotected. */
      g_refusal =
          "BOX64DL_SIGNAL_POLICY has an unknown value (want compat|fast)";
      g_published = 1;
      g_refused = 1;
      return -1;
    }
  }

  /* A malformed BOX64DL_SIGNAL_LIBRARY_RULES is a configuration error: refuse
   * init instead of silently running the affected libraries on the FAST
   * default (a typo like "libfoo.so=compatx" must not quietly become "fast"). */
  if (!rules_valid()) {
    g_refusal =
        "BOX64DL_SIGNAL_LIBRARY_RULES has a malformed rule (want "
        "MATCH=compat|fast, ';'-separated; MATCH containing '/' is an exact "
        "path, otherwise an exact basename)";
    g_published = 1;
    g_refused = 1;
    return -1;
  }

  g_refusal = "";
  g_mode = mode;
  box64dl_sig_dbg_on = (getenv("BOX64DL_SIGNAL_STATS") != NULL) ? 1 : 0;
  box64dl_route_trace = (getenv("BOX64DL_ROUTE_TRACE") != NULL) ? 1 : 0;
  g_publishes = 1;
  g_published = 1;
  return 0;
}

int box64dl_signal_policy_mode(void) { return g_mode; }

const char* box64dl_signal_policy_refusal(void) {
  return g_refusal ? g_refusal : "";
}

/* --- per-library override registry ---------------------------------------- */
/*
 * A bounded table of image ranges, each carrying a non-default policy. Writers
 * hold g_ranges_mu at config/load/unload time only. The fields are atomic so
 * the engine has no data race, but atomics alone do NOT make a concurrent
 * configuration safe: callers must serialize config/load/unload with guest
 * execution (the public contract). Entries are plain values, never elf/library
 * pointers, so a cached entry cannot dangle after dlclose; that path bumps the
 * epoch instead.
 */
#define BOX64DL_MAX_LIB_RANGES 64

typedef struct {
  _Atomic uintptr_t lo;
  _Atomic uintptr_t hi; /* exclusive */
  _Atomic int policy;
} lib_range_t;

static lib_range_t g_ranges[BOX64DL_MAX_LIB_RANGES];
static _Atomic int g_ranges_n;      /* high-water slots ever used */
static _Atomic int g_ranges_active; /* live overrides (n - tombstones) */
static pthread_mutex_t g_ranges_mu = PTHREAD_MUTEX_INITIALIZER;
static _Atomic unsigned g_epoch = 1; /* cache invalidation generation */

int box64dl_signal_lib_rules_active(void) {
  return atomic_load_explicit(&g_ranges_active, memory_order_relaxed) > 0;
}

void box64dl_signal_lib_rules_epoch(void) {
  pthread_mutex_lock(&g_ranges_mu);
  atomic_fetch_add_explicit(&g_epoch, 1, memory_order_relaxed);
  pthread_mutex_unlock(&g_ranges_mu);
}

/* Caller holds g_ranges_mu. Clearing a slot tombstones it (lo = 0) and leaves
 * the high-water count unchanged, so no entry is ever moved: a reader that
 * already matched a slot cannot be redirected onto another library's fields. */
static void ranges_clear_locked(int slot) {
  atomic_store_explicit(&g_ranges[slot].lo, 0, memory_order_release);
  atomic_fetch_sub_explicit(&g_ranges_active, 1, memory_order_relaxed);
  atomic_fetch_add_explicit(&g_epoch, 1, memory_order_relaxed);
}

int box64dl_signal_lib_range_set(uintptr_t lo, uintptr_t hi, int policy) {
  int i, slot = -1, n, existed = 0;
  if (!lo || hi <= lo) return -1;
  if (policy != BOX64DL_POLICY_COMPAT && policy != BOX64DL_POLICY_FAST)
    return -1;

  pthread_mutex_lock(&g_ranges_mu);
  n = atomic_load_explicit(&g_ranges_n, memory_order_relaxed);
  for (i = 0; i < n; ++i) {
    if (atomic_load_explicit(&g_ranges[i].lo, memory_order_acquire) == lo) {
      slot = i;
      existed = 1;
      break;
    }
  }
  /* A range whose policy equals the global default is not an override at all:
   * keep the no-override fast path for the common case. */
  if (policy == g_mode) {
    if (existed) ranges_clear_locked(slot);
    pthread_mutex_unlock(&g_ranges_mu);
    return 0;
  }
  if (!existed) {
    for (i = 0; i < n; ++i)
      if (!atomic_load_explicit(&g_ranges[i].lo, memory_order_relaxed)) {
        slot = i; /* reuse a tombstone */
        break;
      }
    if (slot < 0) {
      if (n >= BOX64DL_MAX_LIB_RANGES) {
        /* Fail loudly: the image would otherwise run on the global FAST default
         * while the configuration looked applied. */
        fprintf(stderr,
                "[libbox64dl] signal library policy table full (%d); override "
                "for %#lx is NOT applied (that image runs the global default)\n",
                BOX64DL_MAX_LIB_RANGES, (unsigned long)lo);
        pthread_mutex_unlock(&g_ranges_mu);
        return -1;
      }
      slot = n;
    }
  }
  /* Fill the slot, publishing lo last (release); grow the high-water count only
   * after the fields are visible. Reads are atomic, but tuple atomicity under a
   * concurrent configuration is NOT promised: callers must keep config / load /
   * unload serialized with guest execution (see the public contract). */
  atomic_store_explicit(&g_ranges[slot].hi, hi, memory_order_relaxed);
  atomic_store_explicit(&g_ranges[slot].policy, policy, memory_order_relaxed);
  atomic_store_explicit(&g_ranges[slot].lo, lo, memory_order_release);
  if (slot == n) atomic_store_explicit(&g_ranges_n, n + 1, memory_order_release);
  if (!existed) atomic_fetch_add_explicit(&g_ranges_active, 1, memory_order_relaxed);
  atomic_fetch_add_explicit(&g_epoch, 1, memory_order_relaxed);
  pthread_mutex_unlock(&g_ranges_mu);
  return 0;
}

void box64dl_signal_lib_range_clear(uintptr_t lo) {
  int i, n;
  if (!lo) return;
  pthread_mutex_lock(&g_ranges_mu);
  n = atomic_load_explicit(&g_ranges_n, memory_order_relaxed);
  for (i = 0; i < n; ++i) {
    if (atomic_load_explicit(&g_ranges[i].lo, memory_order_acquire) == lo) {
      ranges_clear_locked(i);
      break;
    }
  }
  pthread_mutex_unlock(&g_ranges_mu);
}

/* Plain scan; no seqlock. The config contract forbids a concurrent
 * configuration, so a reader never races a slot overwrite in practice; the
 * atomic fields mean it is not a data race even if a signal handler re-enters,
 * but no coherent multi-field snapshot is promised. */
static int ranges_resolve(uintptr_t addr) {
  int i, n = atomic_load_explicit(&g_ranges_n, memory_order_acquire);
  for (i = 0; i < n; ++i) {
    uintptr_t lo = atomic_load_explicit(&g_ranges[i].lo, memory_order_acquire);
    if (!lo || addr < lo) continue;
    if (addr < atomic_load_explicit(&g_ranges[i].hi, memory_order_relaxed))
      return atomic_load_explicit(&g_ranges[i].policy, memory_order_relaxed);
  }
  return g_mode;
}

/* Per-thread direct-mapped cache. addr == 0 means empty (guest addresses are
 * never 0). Refreshed wholesale when the epoch changes. */
#define BOX64DL_POLICY_CACHE_N 8
typedef struct {
  uintptr_t addr;
  int policy;
} policy_cache_ent_t;
static __thread policy_cache_ent_t t_cache[BOX64DL_POLICY_CACHE_N];
static __thread unsigned t_cache_epoch;

int box64dl_signal_policy_for_addr(uintptr_t addr) {
  policy_cache_ent_t* e;
  unsigned ep;

  /* Address 0 is the empty-cache sentinel and never a guest entry. */
  if (!addr) return g_mode;
  if (!atomic_load_explicit(&g_ranges_active, memory_order_relaxed))
    return g_mode;

  ep = atomic_load_explicit(&g_epoch, memory_order_relaxed);
  if (t_cache_epoch != ep) {
    memset(t_cache, 0, sizeof(t_cache));
    t_cache_epoch = ep;
  }
  e = &t_cache[(addr >> 4) & (BOX64DL_POLICY_CACHE_N - 1)];
  if (e->addr == addr) return e->policy;
  e->policy = ranges_resolve(addr);
  e->addr = addr;
  return e->policy;
}

/* --- environment library rules -------------------------------------------- */
/*
 * BOX64DL_SIGNAL_LIBRARY_RULES is a ';'-separated list of MATCH=policy pairs.
 *   - MATCH containing '/' matches the full library path (or its realpath)
 *     exactly;
 *   - any other MATCH matches the exact final path component.
 * No substring/glob/prefix matching, so a rule can never catch a library whose
 * name merely contains the match. The first matching rule wins; an unknown
 * policy makes that rule malformed and it is skipped with a one-time
 * diagnostic.
 */
#define BOX64DL_MAX_LIB_RULES 64
#define BOX64DL_RULE_MATCH_MAX 256

typedef struct {
  char match[BOX64DL_RULE_MATCH_MAX];
  int is_path;
  int policy;
} lib_rule_t;

static lib_rule_t g_rules[BOX64DL_MAX_LIB_RULES];
static int g_rules_n = 0;
static int g_rules_parsed = 0;
static int g_rules_bad = 0;
static pthread_mutex_t g_rules_mu = PTHREAD_MUTEX_INITIALIZER;

static const char* base_name(const char* p) {
  const char* s = strrchr(p, '/');
  return s ? s + 1 : p;
}

static char* trim(char* s) {
  char* e;
  while (*s == ' ' || *s == '\t') ++s;
  e = s + strlen(s);
  while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\n' || e[-1] == '\r'))
    *--e = 0;
  return s;
}

static int rule_policy_from_name(const char* s) {
  if (!strcmp(s, "compat")) return BOX64DL_POLICY_COMPAT;
  if (!strcmp(s, "fast")) return BOX64DL_POLICY_FAST;
  return -1;
}

static void rules_parse_once(void) {
  char* copy;
  char* save = NULL;
  char* tok;
  const char* env;
  if (atomic_load_explicit(&g_rules_parsed, memory_order_acquire)) return;
  pthread_mutex_lock(&g_rules_mu);
  if (g_rules_parsed) {
    pthread_mutex_unlock(&g_rules_mu);
    return;
  }
  env = getenv("BOX64DL_SIGNAL_LIBRARY_RULES");
  if (env && *env) {
    copy = strdup(env);
    if (!copy) {
      pthread_mutex_unlock(&g_rules_mu);
      return;
    }
    for (tok = strtok_r(copy, ";", &save); tok; tok = strtok_r(NULL, ";", &save)) {
      char* eq = strchr(tok, '=');
      char* m;
      char* p;
      int pol;
      if (!eq) {
        g_rules_bad = 1;
        continue;
      }
      *eq = 0;
      m = trim(tok);
      p = trim(eq + 1);
      pol = rule_policy_from_name(p);
      if (!*m || pol < 0 || strlen(m) >= BOX64DL_RULE_MATCH_MAX) {
        g_rules_bad = 1;
        continue;
      }
      if (g_rules_n >= BOX64DL_MAX_LIB_RULES) {
        g_rules_bad = 1;
        break;
      }
      strcpy(g_rules[g_rules_n].match, m);
      g_rules[g_rules_n].is_path = (strchr(m, '/') != NULL);
      g_rules[g_rules_n].policy = pol;
      g_rules_n++;
    }
    free(copy);
  }
  if (g_rules_bad)
    fprintf(stderr,
            "[libbox64dl] BOX64DL_SIGNAL_LIBRARY_RULES: malformed rule(s); "
            "engine init will be refused; syntax is MATCH=compat|fast "
            "separated by ';'\n");
  atomic_store_explicit(&g_rules_parsed, 1, memory_order_release);
  pthread_mutex_unlock(&g_rules_mu);
}

/* Parse once and report whether every rule was well formed. Called at publish
 * (so a bad rule fails init) and, harmlessly, on the first lookup. */
static int rules_valid(void) {
  rules_parse_once();
  return !g_rules_bad;
}

int box64dl_signal_lib_rule_lookup(const char* path) {
  const char* base;
  char real[PATH_MAX];
  const char* realp = NULL;
  int i;
  if (!path || !*path) return -1;
  rules_parse_once();
  if (!g_rules_n) return -1;
  base = base_name(path);
  if (realpath(path, real)) realp = real;
  for (i = 0; i < g_rules_n; ++i) {
    const lib_rule_t* r = &g_rules[i];
    if (r->is_path) {
      if (!strcmp(r->match, path)) return r->policy;
      if (realp && !strcmp(r->match, realp)) return r->policy;
    } else if (!strcmp(r->match, base)) {
      return r->policy;
    }
  }
  return -1;
}

/* --- slow path ------------------------------------------------------------ */

void box64dl_signal_mask_restore(int active_policy, const void* ucntx) {
  /* COMPAT frames are a no-op here: siglongjmp restores the saved mask M0. */
  if (active_policy != BOX64DL_POLICY_FAST) return;

  /* FAST: put back the mask the kernel recorded in the delivered signal's
   * ucontext (M1, the pre-delivery mask). sigprocmask is async-signal-safe and,
   * on Linux/glibc, thread-local. */
  if (!ucntx) return;
  sigprocmask(SIG_SETMASK, &((const ucontext_t*)ucntx)->uc_sigmask, NULL);
  atomic_fetch_add_explicit(&g_restores, 1, memory_order_relaxed);
}
