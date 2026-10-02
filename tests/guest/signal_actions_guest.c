/*
 * x86_64 guest fixture for tests/test_signal_actions.c.
 *
 * PURPOSE
 * -------
 * The signal-ACTION API (include/libbox64_signal_actions.h) promises ONE
 * logical process-wide action per signal, shared by native and guest callers:
 * the app's pre-init native disposition is the initial value, a guest
 * rt_sigaction(2) replaces it, a host box64_signal_action_set_native() replaces
 * it again, and either side can query the other's logical action back. This
 * fixture is the guest half of every one of those round trips.
 *
 * HOW IT REPORTS
 * --------------
 * The host drives it through the engine's scalar call entry (tests/
 * call_compat.h) and each function RETURNS A LOGIC VALUE the host can verify:
 * either a single quantity (a raw syscall result, a count, an address) or a
 * BITMAP whose bits are named next to each function. The guest therefore never
 * has to print anything, and the host never has to trust a guest printf.
 *
 * Built freestanding (-nostdlib -fno-builtin -ffreestanding): every syscall is
 * issued with the `syscall` instruction, and memcpy/memset are hand-written, so
 * the image has no libc dependency at all (a compiler-emitted memcpy() call
 * would leave an unexpected undefined symbol in the guest).
 *
 * NO WINE, NO GS BASE
 * -------------------
 * Everything here runs on a plain Linux-x64 guest thread whose GS base is 0.
 * None of these paths may depend on the guest-context route heuristic in
 * src/signal_route.c (that is only for the four fatal signals): the point of
 * the action API is that a signal's action — not the thread's GS — decides
 * where the delivery goes.
 */

/* ------------------------------------------------------------------ *
 * Kernel ABI numbers and structs (x86_64), freestanding.
 * ------------------------------------------------------------------ */

typedef unsigned long u64;
typedef long i64;

#define SYS_read 0
#define SYS_write 1
#define SYS_mmap 9
#define SYS_mprotect 10
#define SYS_rt_sigaction 13
#define SYS_rt_sigprocmask 14
#define SYS_nanosleep 35
#define SYS_getpid 39
#define SYS_fork 57
#define SYS_wait4 61
#define SYS_kill 62
#define SYS_sigaltstack 131
#define SYS_gettid 186
#define SYS_exit_group 231
#define SYS_tgkill 234

#define G_PROT_READ 1
#define G_PROT_WRITE 2
#define G_PROT_EXEC 4

#define G_MAP_PRIVATE 0x02
#define G_MAP_ANONYMOUS 0x20

/* sa_flags in the values the x86_64 ABI uses, so a raw rt_sigaction(2) and the
 * logical action API agree on the bits an app set.
 *
 * SA_RESTORER (0x04000000) IS part of the logical set. box64 strips it from the
 * KERNEL action it installs (it builds its own synthetic guest frame and never
 * needs a restorer), but the logical action models what the APP registered, and
 * the restorer ADDRESS the app passed is part of that action. A round trip
 * through the logical API must therefore preserve BOTH the SA_RESTORER bit and
 * the restorer address, even though the engine's own kernel action carries
 * neither. g_action_set() uses G_KIND_RAW with an explicit restorer for exactly
 * that case; nothing ever EXECUTES it. */
#define G_SA_NOCLDSTOP 0x00000001UL
#define G_SA_NOCLDWAIT 0x00000002UL
#define G_SA_SIGINFO 0x00000004UL
#define G_SA_ONSTACK 0x08000000UL
#define G_SA_RESTART 0x10000000UL
#define G_SA_NODEFER 0x40000000UL
#define G_SA_RESETHAND 0x80000000UL
#define G_SA_RESTORER 0x04000000UL

/* The logical flag set a round trip must preserve. */
#define G_SA_LOGICAL_MASK                                                    \
  (G_SA_NOCLDSTOP | G_SA_NOCLDWAIT | G_SA_SIGINFO | G_SA_ONSTACK |           \
   G_SA_RESTART | G_SA_NODEFER | G_SA_RESETHAND | G_SA_RESTORER)

#define G_SIG_DFL ((void*)0)
#define G_SIG_IGN ((void*)1)

/* struct sigaction as rt_sigaction(2) takes it: handler, flags, restorer,
 * mask. sigsetsize is 8, so the mask is one 64-bit word. */
struct g_sigaction {
  void* handler;
  u64 flags;
  void* restorer;
  u64 mask;
};

struct g_stack {
  void* ss_sp;
  int ss_flags;
  unsigned long ss_size; /* size_t */
};

struct g_timespec {
  i64 tv_sec;
  i64 tv_nsec;
};

#define G_SS_ONSTACK 1
#define G_SS_DISABLE 2

/* ------------------------------------------------------------------ *
 * Raw syscalls. x86_64: rdi, rsi, rdx, r10, r8, r9.
 * ------------------------------------------------------------------ */

static i64 g_sys6(i64 n, i64 a, i64 b, i64 c, i64 d, i64 e, i64 f) {
  register i64 r10 __asm__("r10") = d;
  register i64 r8 __asm__("r8") = e;
  register i64 r9 __asm__("r9") = f;
  i64 ret;
  __asm__ volatile("syscall"
                   : "=a"(ret)
                   : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8),
                     "r"(r9)
                   : "rcx", "r11", "memory");
  return ret;
}

static i64 g_sys4(i64 n, i64 a, i64 b, i64 c, i64 d) {
  return g_sys6(n, a, b, c, d, 0, 0);
}
static i64 g_sys2(i64 n, i64 a, i64 b) { return g_sys6(n, a, b, 0, 0, 0, 0); }
static i64 g_sys1(i64 n, i64 a) { return g_sys6(n, a, 0, 0, 0, 0, 0); }
static i64 g_sys0(i64 n) { return g_sys6(n, 0, 0, 0, 0, 0, 0); }

static void g_memcpy(void* dst, const void* src, unsigned long n) {
  unsigned char* d = (unsigned char*)dst;
  const unsigned char* s = (const unsigned char*)src;
  unsigned long i;
  for (i = 0; i < n; ++i) d[i] = s[i];
}

static void g_memset(void* dst, int c, unsigned long n) {
  unsigned char* d = (unsigned char*)dst;
  unsigned long i;
  for (i = 0; i < n; ++i) d[i] = (unsigned char)c;
}

static i64 g_gettid(void) { return g_sys0(SYS_gettid); }
static i64 g_getpid(void) { return g_sys0(SYS_getpid); }

static i64 g_raw_action(i64 sig, const struct g_sigaction* act,
                        struct g_sigaction* old) {
  return g_sys4(SYS_rt_sigaction, sig, (i64)act, (i64)old, 8);
}

static i64 g_raw_mask(i64 how, const u64* set, u64* old) {
  return g_sys4(SYS_rt_sigprocmask, how, (i64)set, (i64)old, 8);
}

static u64 g_bit(i64 sig) { return 1UL << (sig - 1); }

/* Forward declarations: the re-registration probes below run before the
 * installation helpers are defined, and the handlers are called by the kernel
 * with the C ABI the host sees. */
static i64 g_raw_action(i64 sig, const struct g_sigaction* act,
                        struct g_sigaction* old);
static i64 g_raw_mask(i64 how, const u64* set, u64* old);
static u64 g_bit(i64 sig);
long g_action_set(long sig, long kind, long handler, long restorer, long flags,
                  long mask);
long g_raise(long sig);

/* ------------------------------------------------------------------ *
 * State shared between the host-driven calls and the guest handlers.
 * ------------------------------------------------------------------ */

static volatile u64 g_hits_a;    /* 1-arg handler A entered */
static volatile u64 g_hits_b;    /* 3-arg (SA_SIGINFO) handler B entered */
static volatile u64 g_hits_reset;/* RESETHAND probe handler entered */
static volatile u64 g_handler_mask;  /* raw rt_sigprocmask inside handler A */
static volatile u64 g_handler_frame; /* &local inside handler A */
static volatile i64 g_handler_sig;   /* sig as the handler received it */
static volatile i64 g_handler_tid;   /* gettid() inside handler A */
static volatile u64 g_altstack_in_use; /* SS_ONSTACK seen inside the handler */
static volatile u64 g_mprotect_fixed;  /* guest handler repaired a page */
/* State for the fault-inside-a-native-callback case (G_KIND_NATFAULT). */
static volatile u64 g_nf_fix_addr;
static volatile u64 g_nf_fix_len;
static volatile u64 g_nf_fixed;

/* The action the guest read back before replacing it, for the
 * round trip ("call the old bridge, then restore it"). */
static struct g_sigaction g_saved;
static volatile u64 g_saved_valid;
static volatile u64 g_roundtrip_bm;

/* Alt stack owned by the guest (a plain .bss buffer, so the guest's own
 * sigaltstack address is an ordinary guest address). */
static unsigned char g_altstack_area[64 * 1024];

/* Forward declaration: the SIGSEGV repair handler below may raw-mprotect a
 * page. That path is box64's my_mprotect(), which calls the HOST mprotect. */
static long g_mprotect(long addr, long size, long prot);

/* ------------------------------------------------------------------ *
 * The guest ucontext the ENGINE hands a handler, and what editing it does.
 *
 * box64 does not deliver the host ucontext to a guest handler: it builds an
 * x86_64 guest ucontext (x64_ucontext_t in the vendored signal_private.h) and,
 * after the handler returns, compares that structure against a copy taken
 * before the call. ANY difference makes box64 resume the emulator with
 * siglongjmp instead of a plain return, i.e. a DIFFERENT path from the handler's
 * ordinary rt_sigprocmask(2) additions: bit 4 of g_uc_edit_probe's bitmap
 * measures the latter, the mask bits measure the former.
 *
 * TARGET CONTRACT: after the resume, BOTH policies must left the guest ucontext
 * mask edit (and any other ucontext edit) intact: the app's handler said what
 * the mask must be, and a resume that silently discarded it would lose the
 * handler's own decision. An earlier COMPAT-vs-FAST split restored the
 * setjmp-time mask (M0) or the delivered one (M1) and could therefore drop a
 * handler-made uc_sigmask edit; that split is NOT the contract and these cases
 * exist to catch it, so the assertions below require the SAME result under
 * BOX64DL_SIGNAL_POLICY=fast and =compat.
 *
 * The x86_64 offsets below are fixed by the kernel ABI and match glibc's
 * ucontext_t layout: uc_mcontext at 40, gregs at 40 with
 * REG_RIP = 16, REG_RSP = 14 and REG_R11 = 3, and uc_sigmask at 296 as a
 * 128-byte sigset whose FIRST WORD is the same 8-byte kernel encoding the
 * fixture uses everywhere else (bit i = signal i+1). Only that first word is
 * touched, and box64's x64_sigset_t begins with the same `unsigned long sig[]`,
 * so the edit means the same thing to the engine as it does to the host. */
#define G_UC_SIGMASK_OFF 296u
#define G_UC_RIP_OFF 168u
#define G_UC_RSP_OFF 160u
#define G_UC_R11_OFF 64u

/* Handlers referenced by g_action_set()'s kind switch but defined further down
 * (they depend on state declared later in the file). */
static void g_handler_natfault(int sig, void* info, void* ucv);
static void g_handler_chain(int sig, void* info, void* ucv);

/* Handler-A behaviour control (defined again below, declared here because the
 * handler body reads it). */
static volatile u64 g_uc_edit;
static volatile u64 g_uc_mask;
static volatile u64 g_uc_pre_seen;
static volatile u64 g_uc_extra_block;

/* ------------------------------------------------------------------ *
 * Guest handlers.
 * ------------------------------------------------------------------ */

/* 1-arg handler (installed WITHOUT SA_SIGINFO). Records the raw HOST mask in
 * force while it runs (via rt_sigprocmask(2), which box64 passes through to the
 * real syscall) and the address of one of its own locals: the host uses the
 * latter to prove an SA_ONSTACK delivery really landed on the guest alt stack.
 *
 * With g_uc_edit == 0 it deliberately leaves its ucontext alone, so the
 * non-fatal delivery path is a plain return and the kernel's own post-handler
 * mask restore is what the host observes. The other modes change the delivered
 * context on purpose, which is what makes box64 take the siglongjmp resume path
 * (and therefore the FAST/COMPAT mask difference). */
static void g_handler_a(int sig, void* info, void* ucv) {
  u64 local = 0;
  unsigned char* p = (unsigned char*)&local;
  g_hits_a++;
  g_handler_sig = sig;
  g_handler_tid = g_gettid();
  g_handler_frame = (u64)(unsigned long)p;
  /* Read the mask we were entered with (before any change of our own). This is
   * what the APPLICATION had blocked, and it is what g_uc_edit_probe's "pre"
   * bits refer to. */
  g_handler_mask = 0;
  (void)g_raw_mask(0 /*SIG_BLOCK*/, 0, (u64*)&g_handler_mask);

  /* Optional ordinary handler-side mask change: an rt_sigprocmask(2) addition
   * the handler makes itself. These bits are ORDINARY handler additions and must
   * NOT survive the return, unlike an edit of the delivered ucontext. */
  if (g_uc_extra_block) {
    u64 add = g_uc_extra_block;
    (void)g_raw_mask(0 /*SIG_BLOCK*/, &add, 0);
    /* Re-read so g_handler_mask describes the state the handler actually ran
     * with, i.e. including its own addition. */
    (void)g_raw_mask(0, 0, (u64*)&g_handler_mask);
  }

  /* Optional edit of the DELIVERED guest ucontext. box64 compares this
   * structure after the handler returns; any difference sends it down
   * siglongjmp, where the FAST policy restores the delivered mask and COMPAT
   * restores the setjmp-time mask. */
  if (g_uc_edit && ucv) {
    unsigned char* uc = (unsigned char*)ucv;
    switch ((int)g_uc_edit) {
      case 1: { /* OR in uc_mask */
        u64* m = (u64*)(uc + G_UC_SIGMASK_OFF);
        *m |= (u64)g_uc_mask;
        break;
      }
      case 2: { /* CLEAR uc_mask */
        u64* m = (u64*)(uc + G_UC_SIGMASK_OFF);
        *m &= ~((u64)g_uc_mask);
        break;
      }
      case 3: { /* perturb a dead caller-saved register (R11) */
        u64* r = (u64*)(uc + G_UC_R11_OFF);
        *r ^= 0x5a5a;
        break;
      }
      case 4: { /* COMBINED: change the emulator-visible register state AND the
                 * mask. Either edit alone may legitimately be copied onto the
                 * native ucontext and returned from normally; together they put
                 * the delivery on the DEFINITIVE changed-state path, which is
                 * what a case that wants to prove "the resume happened" must
                 * drive. */
        u64* r = (u64*)(uc + G_UC_R11_OFF);
        u64* m = (u64*)(uc + G_UC_SIGMASK_OFF);
        *r ^= 0x5a5a;
        *m |= (u64)g_uc_mask;
        break;
      }
      default:
        break;
    }
  }
  (void)info;
}

/* 3-arg handler (installed WITH SA_SIGINFO), for the siginfo round trip. */
static void g_handler_b(int sig, void* info, void* ucv) {
  g_hits_b++;
  g_handler_sig = sig;
  g_handler_mask = 0;
  (void)g_raw_mask(0, 0, (u64*)&g_handler_mask);
  if (info) {
    /* si_signo is the first int of siginfo_t on x86_64. */
    g_altstack_in_use = (u64)(unsigned)((int*)info)[0];
  }
  (void)ucv;
}

/* Handler for the RESETHAND (one-shot) probe: it does not touch the ucontext,
 * so box64 resumes the guest normally after it returns. */
static void g_handler_reset(int sig) {
  g_hits_reset++;
  g_handler_sig = sig;
}

/* The "newer registration" variant: a distinct one-shot handler, so a probe can
 * tell WHICH registration actually fired. */
static volatile u64 g_hits_reset2;
static void g_handler_reset2(int sig) {
  g_hits_reset2++;
  g_handler_sig = sig;
}

/* The x86_64 kernel returns from a signal frame through
 * `ka->sa.sa_restorer`; with a NULL restorer that return goes to address 0.
 * box64 does NOT rely on it: my_sigaction()/my_syscall_rt_sigaction() record
 * `restorer` only when the guest passed SA_RESTORER, and build their own
 * synthetic guest frame for the handler, so a restorer-0 action is what the
 * existing fixture (tests/guest/sig_guest.c) uses and what box64 expects. The
 * SA_RESTORER *value* is still part of the logical action and is round-tripped
 * by the `roundtrip` case, which never executes it. */


#define G_KIND_DFL 0
#define G_KIND_IGN 1
#define G_KIND_GUEST_A 2   /* 1-arg handler A */
#define G_KIND_GUEST_B 3   /* 3-arg handler B (forces SA_SIGINFO) */
#define G_KIND_RAW 4       /* handler/restorer taken from the arguments */
#define G_KIND_PAGEFIX 5   /* 1-arg SIGSEGV page repair handler */
#define G_KIND_CHLD 6      /* 1-arg SIGCHLD counting handler */
#define G_KIND_RESET 7     /* 1-arg one-shot probe handler */
#define G_KIND_RESET2 8    /* a DISTINCT one-shot handler (re-registration) */
#define G_KIND_NATFAULT 9  /* 3-arg handler for a fault raised INSIDE a native
                            * C callback: repairs the page and perturbs guest R11 */
#define G_KIND_CHAIN 10    /* same, but ALSO chains to the previously saved native
                            * action, passing the guest ucontext through */

/* RESETHAND re-registration, the two properties a concurrent new registration
 * must not break:
 *   - the one-shot fires EXACTLY once per installation, and
 *   - the OLD (already-fired) variant must not override the NEW registration.
 * Bitmap:
 *   bit 0  the first registration's handler ran exactly once
 *   bit 1  the action was DEFAULT after that one delivery
 *   bit 2  the second registration's handler ran exactly once
 *   bit 3  the FIRST handler did not run again for the second delivery
 *         (the old variant did not override the new registration)
 *   bit 4  the action was DEFAULT after the second delivery */
long g_one_shot_rereg_probe(long sig) {
  struct g_sigaction q;
  u64 bm = 0;

  g_hits_reset = 0;
  g_hits_reset2 = 0;
  g_memset(&q, 0, sizeof(q));

  /* First registration + one delivery. */
  if (g_action_set(sig, G_KIND_RESET, 0, 0, G_SA_RESETHAND, 0) != 0) return -1;
  (void)g_raise(sig);
  if (g_hits_reset == 1) bm |= 1;
  if (g_raw_action(sig, 0, &q) == 0 && (q.handler == G_SIG_DFL || q.handler == G_SIG_IGN))
    bm |= 2;

  /* Second, distinct registration + one delivery. */
  if (g_action_set(sig, G_KIND_RESET2, 0, 0, G_SA_RESETHAND, 0) != 0) return -2;
  (void)g_raise(sig);
  if (g_hits_reset2 == 1) bm |= 4;
  if (g_hits_reset == 1) bm |= 8;
  g_memset(&q, 0, sizeof(q));
  if (g_raw_action(sig, 0, &q) == 0 && (q.handler == G_SIG_DFL || q.handler == G_SIG_IGN))
    bm |= 16;
  return (long)bm;
}

/* One RESETHAND (re-)registration, for the CONCURRENT re-registration case.
 * Installs the variant the caller names (7 or 8) and reports:
 *   0            installed
 *   -EWOULDBLOCK the engine's controlled setter was busy (TRANSIENT, expected
 *                under concurrency; the caller retries or counts it separately)
 *   -errno       a hard refusal
 * The guest cannot see the engine's errno directly through the raw syscall, so
 * it maps the raw -EAGAIN/-EBUSY shape onto -EWOULDBLOCK and leaves everything
 * else as the raw negative value: the host only needs to distinguish "busy" from
 * "really refused", and both arrive as a negative errno. */
long g_one_shot_rereg_one(long sig) {
  i64 r;
  g_hits_reset = 0;
  g_hits_reset2 = 0;
  r = g_action_set(sig, G_KIND_RESET2, 0, 0, G_SA_RESETHAND, 0);
  if (r == 0) return 0;
  if (r == -11 /*EAGAIN*/ || r == -16 /*EBUSY*/) return -11;
  return (long)r;
}

/* Quiescent check after a concurrent phase: install a NEW RESETHAND action and
 * report which handler the next single delivery actually ran (1 = the new one,
 * 0 = the stale old one). */
long g_one_shot_which(long sig) {
  g_hits_reset = 0;
  g_hits_reset2 = 0;
  if (g_action_set(sig, G_KIND_RESET2, 0, 0, G_SA_RESETHAND, 0) != 0) return -1;
  (void)g_raise(sig);
  if (g_hits_reset2 == 1 && g_hits_reset == 0) return 1;
  return 0;
}

/* BOUNDED-RETRY GUARD, shared by every fault-repairing handler in this fixture.
 *
 * A handler that cannot repair its page and then RETURNS makes the faulting
 * instruction re-execute, so the handler runs again: an infinite loop that
 * wedges the thread. The host already bounds every such path with a timed join,
 * but a loud, immediate failure is far more diagnosable than a 6-second wedge,
 * and a wedged worker tells you nothing about WHY the fault was unrepairable.
 *
 * After FAULT_RETRY_MAX unsuccessful attempts on the same address the handler
 * marks the attempt hopeless, restores SIG_DFL for the signal with the raw
 * syscall and returns. The next execution of the faulting instruction is then a
 * real default disposition: the process terminates with the signal, which the
 * build sees as a hard failure instead of a hang. The flag is also reported to
 * the host, so a case can name the reason.
 *
 * FAULT_RETRY_MAX is small: a repair that needs more than a handful of attempts
 * is not a repair. */
#define FAULT_RETRY_MAX 8

static volatile u64 g_fault_attempts;
static volatile u64 g_fault_hopeless;
static volatile u64 g_fault_addr_watched;

/* Returns 1 when the caller may keep repairing, 0 when it must give up. */
static int g_fault_guard(long addr) {
  if ((u64)(unsigned long)addr != g_fault_addr_watched) {
    g_fault_addr_watched = (u64)(unsigned long)addr;
    g_fault_attempts = 0;
    g_fault_hopeless = 0;
  }
  if (++g_fault_attempts > FAULT_RETRY_MAX) {
    struct g_sigaction dfl;
    g_fault_hopeless = 1;
    /* SIG_DFL for this signal: the next execution of the faulting instruction
     * terminates the process loudly instead of looping. */
    dfl.handler = G_SIG_DFL;
    dfl.flags = 0;
    dfl.restorer = 0;
    dfl.mask = 0;
    (void)g_raw_action(11 /*SIGSEGV*/, &dfl, 0);
    return 0;
  }
  return 1;
}

/* SIGSEGV handler that repairs a read-only page with the raw mprotect syscall
 * and returns: the guest fault must be resumed at the faulting instruction,
 * with no native-PC fiction and no change to the delivered ucontext. */
static volatile u64 g_fix_addr;
static volatile u64 g_fix_len;

static void g_handler_pagefix(int sig) {
  g_hits_a++;
  (void)sig;
  if (!g_fix_addr || !g_fix_len) return;
  if (!g_fault_guard((long)g_fix_addr)) return;
  if (g_mprotect((long)g_fix_addr, (long)g_fix_len,
                 G_PROT_READ | G_PROT_WRITE) == 0)
    g_mprotect_fixed = 1;
}

long g_fault_hopeless_count(void) { return (long)g_fault_hopeless; }
long g_fault_attempt_count(void) { return (long)g_fault_attempts; }

/* SIGCHLD handler: only counts. */
static void g_handler_chld(int sig) {
  g_hits_b++;
  (void)sig;
}

/* ------------------------------------------------------------------ *
 * A fault raised by NATIVE C code while the guest is executing: the mirror of
 * native-fault and the direction that can corrupt a C frame.
 *
 * The native callback below deliberately dereferences a PROT_NONE page. Because
 * a GUEST call is active (the guest invoked the callback through a bridge), the
 * engine may route the fault to the GUEST action. That is the dangerous case:
 * if the engine resumed by a guest longjmp it would unwind PAST the native C
 * callback frame, so the callback's stack cookie would be destroyed and the
 * guest would never get its return value. This case asserts the opposite: the
 * guest handler may repair the page, the guest may perturb its own ucontext, and
 * the native callback must still RETURN NORMALLY to its caller.
 * ------------------------------------------------------------------ */

/* The guest-side handler for G_KIND_NATFAULT. It repairs the host page the
 * native callback touched (raw mprotect through the guest syscall path, which
 * reaches the host mprotect) and perturbs R11 in the delivered guest ucontext so
 * the engine's changed-context siglongjmp path is genuinely exercised.
 *
 * It never prints: a signal handler must not printf. */
static void g_handler_natfault(int sig, void* info, void* ucv) {
  g_hits_b++;
  (void)info;
  (void)sig;
  if (g_nf_fix_addr && g_nf_fix_len) {
    if (!g_fault_guard((long)g_nf_fix_addr)) return;
    if (g_mprotect((long)g_nf_fix_addr, (long)g_nf_fix_len,
                   G_PROT_READ | G_PROT_WRITE) == 0) {
      g_nf_fixed = 1;
      /* Only perturb the ucontext once the page is actually repairable, so a
       * hopeless fault still fails fast instead of looping. */
      if (ucv) {
        u64* r = (u64*)((unsigned char*)ucv + G_UC_R11_OFF);
        *r ^= 0x5a5a;
      }
    }
  }
}

/* ------------------------------------------------------------------ *
 * Action installation / query (guest side).
 * ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ *
 * Action installation / query (guest side).
 * ------------------------------------------------------------------ */

/* Install an action with raw kernel-encoded flags/mask. Returns the raw
 * rt_sigaction(2) result (0 on success). handler/restorer are only read for
 * G_KIND_RAW. */
long g_action_set(long sig, long kind, long handler, long restorer, long flags,
                  long mask) {
  struct g_sigaction sa;
  sa.handler = G_SIG_DFL;
  sa.flags = (u64)flags;
  sa.restorer = (void*)(unsigned long)restorer;
  sa.mask = (u64)mask;
  switch (kind) {
    case G_KIND_DFL: sa.handler = G_SIG_DFL; sa.flags = 0; sa.restorer = 0; sa.mask = 0; break;
    case G_KIND_IGN: sa.handler = G_SIG_IGN; sa.flags = 0; sa.restorer = 0; sa.mask = 0; break;
    case G_KIND_GUEST_A: sa.handler = (void*)g_handler_a; break;
    case G_KIND_GUEST_B: sa.handler = (void*)g_handler_b; sa.flags |= G_SA_SIGINFO; break;
    case G_KIND_PAGEFIX: sa.handler = (void*)g_handler_pagefix; break;
    case G_KIND_CHLD: sa.handler = (void*)g_handler_chld; break;
    case G_KIND_RESET: sa.handler = (void*)g_handler_reset; break;
    case G_KIND_RESET2: sa.handler = (void*)g_handler_reset2; break;
    case G_KIND_NATFAULT: sa.handler = (void*)g_handler_natfault;
      sa.flags |= G_SA_SIGINFO; break;
    case G_KIND_CHAIN: sa.handler = (void*)g_handler_chain;
      sa.flags |= G_SA_SIGINFO; break;
    case G_KIND_RAW: sa.handler = (void*)(unsigned long)handler; sa.restorer = (void*)(unsigned long)restorer; break;
    default: return -22; /* -EINVAL */
  }
  return (long)g_raw_action(sig, &sa, 0);
}

/* Raw query: writes 32 bytes {handler, flags, restorer, mask} into out. */
long g_action_query(long sig, void* out) {
  struct g_sigaction got;
  i64 ret;
  g_memset(&got, 0, sizeof(got));
  ret = g_raw_action(sig, 0, &got);
  if (out) g_memcpy(out, &got, sizeof(got));
  return (long)ret;
}

/* Raw query with a caller-chosen sigsetsize, to prove the engine refuses a
 * size the x86_64 kernel ABI does not accept. */
long g_action_query_size(long sig, void* out, long sigsetsize) {
  struct g_sigaction got;
  g_memset(&got, 0, sizeof(got));
  {
    i64 ret = g_sys4(SYS_rt_sigaction, sig, 0, (i64)&got, sigsetsize);
    if (out) g_memcpy(out, &got, sizeof(got));
    return (long)ret;
  }
}

/* Query, re-install the SAME value verbatim, query again; return a bitmap:
 *   bit 0 handler round-tripped
 *   bit 1 flags   round-tripped
 *   bit 2 restorer round-tripped
 *   bit 3 mask    round-tripped
 * `out` (optional) receives the 32-byte first query. This is the guest half of
 * "call the old (native) action and restore it": for a native action the value
 * the guest gets is a stable guest-callable bridge, and re-installing it must
 * land back on the native action, not on a guest one. */
long g_action_roundtrip(long sig, void* out) {
  struct g_sigaction a, b;
  u64 bm = 0;
  g_memset(&a, 0, sizeof(a));
  g_memset(&b, 0, sizeof(b));
  if (g_raw_action(sig, 0, &a) != 0) return -1;
  if (out) g_memcpy(out, &a, sizeof(a));
  if (g_raw_action(sig, &a, 0) != 0) return -2;
  if (g_raw_action(sig, 0, &b) != 0) return -3;
  if (a.handler == b.handler) bm |= 1;
  if (a.flags == b.flags) bm |= 2;
  if (a.restorer == b.restorer) bm |= 4;
  if (a.mask == b.mask) bm |= 8;
  g_roundtrip_bm = bm;
  return (long)bm;
}

/* Deliver `sig` with the 1-arg guest handler installed with `flags`, letting the
 * handler make `uc_edit`, and report a bitmap measured AFTER the delivery
 * returned. Inputs are DELIBERATELY SEPARATE:
 *
 *   pre_mask     bits the HOST blocked BEFORE the call. These are the
 *                application's own mask and must be PRESERVED across the
 *                delivery, unless the handler's ucontext edit explicitly clears
 *                them.
 *   extra_block  bits the HANDLER blocks ITSELF, inside the handler, with an
 *                ordinary rt_sigprocmask(2). These are ordinary handler-added
 *                bits: they must NOT survive the return, exactly like the
 *                automatic self-block. (That is what tests/test_signal_actions.c's
 *                flags-mask case asserts for both sides.)
 *
 * Conflating the two was a bug: "the handler blocked X" and "the application had
 * X blocked" have OPPOSITE required outcomes, so one input cannot express both.
 *
 * uc_edit:
 *   0  leave the delivered ucontext alone
 *   1  OR  `uc_mask` into uc_sigmask
 *   2  CLEAR `uc_mask` from uc_sigmask
 *   3  XOR the dead R11 register
 *   4  R11 XOR AND OR `uc_mask`  (COMBINED: register + mask)
 *
 * Bitmap:
 *   bit 0  the handler ran
 *   bit 1  the handler OBSERVED the pre_mask bits blocked
 *   bit 2  the handler OBSERVED the automatic self-block of `sig`
 *   bit 3  the handler OBSERVED the extra_block bits blocked (its own SIG_BLOCK
 *          took effect while it ran; only meaningful when extra_block != 0)
 *   bit 4  after: every uc_mask bit is BLOCKED (edit modes 1 and 4)
 *   bit 5  after: every uc_mask bit is CLEAR   (edit mode 2)
 *   bit 6  after: the extra_block bits did NOT survive (ordinary handler-added
 *          bits are cleared; only meaningful when extra_block != 0)
 *   bit 7  after: the pre_mask bits the uc did NOT clear are still BLOCKED
 *   bit 8  after: the automatic self-block of `sig` is gone
 *   bit 9  the handler ran on this thread
 *
 * Every "after" bit is a statement about the two frozen requirements: a guest
 * UCONTEXT edit survives, an ordinary handler sigprocmask addition does not, and
 * the application's pre-existing mask is left alone. */
long g_uc_edit_probe(long sig, long flags, long uc_edit, long uc_mask,
                     long pre_mask, long extra_block) {
  u64 after = 0;
  u64 bm = 0;
  u64 add_bit = 1UL << (sig - 1);
  u64 edit_bits = (u64)uc_mask;
  u64 pre = (u64)pre_mask;
  u64 extra = (u64)extra_block;
  u64 pre_expected;

  g_hits_a = 0;
  g_handler_mask = 0;
  g_uc_edit = (u64)uc_edit;
  g_uc_mask = (u64)uc_mask;
  g_uc_extra_block = extra;

  /* SA_SIGINFO MUST be set for any uc-edit mode: g_handler_a edits its THIRD
   * argument, which only exists under the explicit 3-argument ABI. A logical
   * 1-arg handler is not guaranteed to receive a ucontext at all, so relying on
   * the engine always passing one would test an accident rather than the
   * documented SA_SIGINFO contract. The mask probes that never touch the
   * ucontext (uc_edit == 0) stay on whatever flags the caller chose. */
  if (g_uc_edit)
    flags |= G_SA_SIGINFO;
  if (g_action_set(sig, G_KIND_GUEST_A, 0, 0, flags, 0) != 0) return -1;
  (void)g_raise(sig);
  if (g_raw_mask(0, 0, &after) != 0) return -2;

  /* What the handler observed while it ran. */
  if (g_hits_a) bm |= 1;
  if (pre && (g_handler_mask & pre) == pre) bm |= 2;
  if (g_handler_mask & add_bit) bm |= 4;
  if (extra && (g_handler_mask & extra) == extra) bm |= 8;

  /* What survived the return. */
  if (edit_bits && (edit_bits & after) == edit_bits) bm |= 16;
  if (edit_bits && (edit_bits & after) == 0) bm |= 32;
  if (extra && (after & extra) == 0) bm |= 64;

  /* The pre bits the uc explicitly cleared are EXPECTED to be gone; every other
   * pre bit must still be there. */
  pre_expected = pre;
  if ((int)uc_edit == 2) pre_expected &= ~edit_bits;
  if (pre_expected == 0 || (after & pre_expected) == pre_expected) bm |= 128;
  if ((after & add_bit) == 0) bm |= 256;
  if (g_handler_tid == g_gettid()) bm |= 512;
  return (long)bm;
}

/* SA_RESTORER round trip. The app registers an action with SA_RESTORER and a
 * REAL restorer ADDRESS; box64 strips both from the KERNEL action it installs,
 * but the logical action must preserve them, so a query must report SA_RESTORER
 * set and the same restorer address, and re-installing that value must not lose
 * either. `restorer` is the address to register (the harness passes its own
 * guest-callable entry, or any address: this test never EXECUTES it -- the
 * delivery is an ordinary return, and the assertion is about the value that
 * survives the round trip).
 *
 * Returns a bitmap:
 *   bit 0  the installation succeeded
 *   bit 1  the first query reported SA_RESTORER set
 *   bit 2  the first query reported the exact restorer address
 *   bit 3  the re-install of that value succeeded
 *   bit 4  the second query still reports SA_RESTORER set
 *   bit 5  the second query still reports the exact restorer address
 *   bit 6  the handler and the other logical flags are unchanged too */
long g_restorer_roundtrip(long sig, long restorer) {
  struct g_sigaction a, b;
  u64 bm = 0;
  u64 logical = (u64)(G_SA_SIGINFO | G_SA_RESTART | G_SA_RESTORER);

  g_memset(&a, 0, sizeof(a));
  g_memset(&b, 0, sizeof(b));

  if (g_action_set(sig, G_KIND_RAW, (long)(unsigned long)g_handler_a,
                   restorer, (long)logical, 0) != 0)
    return bm;
  bm |= 1;

  if (g_raw_action(sig, 0, &a) != 0) return bm;
  if (a.flags & G_SA_RESTORER) bm |= 2;
  if ((u64)(unsigned long)a.restorer == (u64)restorer) bm |= 4;

  if (g_raw_action(sig, &a, 0) != 0) return bm;
  bm |= 8;

  if (g_raw_action(sig, 0, &b) != 0) return bm;
  if (b.flags & G_SA_RESTORER) bm |= 16;
  if ((u64)(unsigned long)b.restorer == (u64)restorer) bm |= 32;
  if (b.handler == a.handler && b.flags == a.flags && b.mask == a.mask) bm |= 64;
  return (long)bm;
}

/* Two consecutive raw queries of the same signal; return 1 when every one of
 * the four fields is identical (the "stable bridge" property). */
long g_action_query_stable(long sig) {
  struct g_sigaction a, b;
  g_memset(&a, 0, sizeof(a));
  g_memset(&b, 0, sizeof(b));
  if (g_raw_action(sig, 0, &a) != 0) return -1;
  if (g_raw_action(sig, 0, &b) != 0) return -2;
  if (a.handler != b.handler) return 0;
  if (a.flags != b.flags) return 0;
  if (a.restorer != b.restorer) return 0;
  if (a.mask != b.mask) return 0;
  return 1;
}

/* Save the current action (query only) and report the handler: the host compares
 * it against the raw native address and against box64's own dispatcher. */
long g_action_save(long sig) {
  g_memset(&g_saved, 0, sizeof(g_saved));
  if (g_raw_action(sig, 0, &g_saved) != 0) return -1;
  g_saved_valid = 1;
  return (long)(unsigned long)g_saved.handler;
}

long g_saved_handler(void) { return (long)(unsigned long)g_saved.handler; }
long g_saved_flags(void) { return (long)g_saved.flags; }
long g_saved_mask(void) { return (long)g_saved.mask; }
long g_saved_restorer(void) { return (long)(unsigned long)g_saved.restorer; }

/* Re-install exactly what g_action_save() read (the guest's "restore the old
 * action" step). Returns the raw result; -1 when nothing was saved. */
long g_action_restore_saved(long sig) {
  if (!g_saved_valid) return -2;
  return (long)g_raw_action(sig, &g_saved, 0);
}

/* Call the saved action the way its own flags say it must be called: 3
 * arguments when SA_SIGINFO is set, 1 otherwise. This is the GUEST half of
 * "the guest calls the old action (chaining)": for a NATIVE action the value
 * g_action_save() returned is a stable guest-callable bridge, so this call
 * re-enters the ORIGINAL native function. Returns 0 when it returned, -1 when
 * nothing was saved, -2 on an unknown arity. */
long g_action_call_saved(long sig) {
  if (!g_saved_valid || !g_saved.handler) return -1;
  if (g_saved.handler == G_SIG_DFL || g_saved.handler == G_SIG_IGN) return -1;
  if (g_saved.flags & G_SA_SIGINFO) {
    void (*fn3)(int, void*, void*) = (void (*)(int, void*, void*))g_saved.handler;
    fn3((int)sig, 0, 0);
    return 1;
  } else {
    void (*fn1)(int) = (void (*)(int))g_saved.handler;
    fn1((int)sig);
    return 1;
  }
}

/* The handler VALUE the guest read, as an integer, for the host to compare
 * against the raw native address and against box64's dispatcher. */
long g_action_saved_handler(void) { return (long)(unsigned long)g_saved.handler; }

/* ------------------------------------------------------------------ *
 * The fault-inside-a-native-callback driver.
 *
 * Arming: map a PROT_NONE page, install the G_KIND_NATFAULT guest handler for
 * SIGSEGV, remember the page for the handler, and return a bitmap:
 *   bit 0  the page was mapped
 *   bit 1  the action was installed
 *   bit 2  building the 3-arg guest-callable bridge for the callback worked
 * Invoking: call the bridged native callback with the page address. The callback
 * faults; the guest handler repairs the page; the callback must then return its
 * own success value. The bitmap the caller sees:
 *   bit 0  the callback returned (did NOT unwind past its C frame)
 *   bit 1  the callback's own stack cookie was intact at return
 *   bit 2  the handler ran
 *   bit 3  the handler's mprotect succeeded
 * The cookie check is done INSIDE the callback, in C, on either side of the
 * dereference, which is what makes "the C frame survived" a real assertion: if
 * the engine resumed through the guest longjmp the function would never reach
 * the second cookie comparison.
 * ------------------------------------------------------------------ */

/* The GUEST invokes the bridged HOST callback through this entry, exactly the
 * way a guest would call any native function. The callback (native C code, in
 * the harness) is what dereferences the poisoned page and holds the stack
 * cookie; nothing here can fake that, because the cookie lives on the NATIVE C
 * frame the engine must not unwind.
 *
 * BOTH arguments are forwarded, in the callback's own order
 * (cookie, page, ...). Passing only one would leave the callback's `page`
 * parameter holding whatever the bridge left in the second argument register,
 * so the deliberate fault would land on an address the guest repair handler
 * cannot fix and the case would die of its own driver, not of the engine. */
long g_natfault_invoke(long bridge, long page, long cookie) {
  long (*f)(long, long) = (long (*)(long, long))(unsigned long)bridge;
  if (!f) return -1;
  return f(cookie, page);
}

/* Arm a PROT_NONE page and the guest handler. Bitmap:
 *   bit 0  the page was mapped
 *   bit 1  the guest handler was installed
 */
long g_natfault_prepare(void) {
  u64 bm = 0;
  unsigned char* p;
  long page = 4096;
  p = (unsigned char*)g_sys6(SYS_mmap, 0, page, 0 /*PROT_NONE*/,
                             G_MAP_PRIVATE | G_MAP_ANONYMOUS, -1, 0);
  if ((long)(unsigned long)p <= 0) return (long)(unsigned long)p;
  bm |= 1;
  g_nf_fix_addr = (u64)(unsigned long)p;
  g_nf_fix_len = (u64)page;
  g_nf_fixed = 0;
  g_hits_b = 0;
  if (g_action_set(11 /*SIGSEGV*/, G_KIND_NATFAULT, 0, 0, 0, 0) != 0) return -2;
  bm |= 2;
  return (long)bm;
}

/* The address the native callback must dereference. */
long g_natfault_page(void) { return (long)g_nf_fix_addr; }
long g_natfault_fixed(void) { return (long)g_nf_fixed; }
long g_natfault_hits(void) { return (long)g_hits_b; }

/* The chaining form: repair the page, then call the action that was current
 * BEFORE this guest handler was installed — a NATIVE handler — with the guest
 * ucontext the engine handed us. Per the header the guest worker bridges that
 * call so the native handler receives a REAL native ucontext_t, not the guest
 * one. The host's native handler asserts that.
 *
 * It never prints: a signal handler must not printf. */
static volatile u64 g_chain_hits;
static void g_handler_chain(int sig, void* info, void* ucv) {
  g_hits_b++;
  if (g_nf_fix_addr && g_nf_fix_len) {
    if (!g_fault_guard((long)g_nf_fix_addr)) return;
    if (g_mprotect((long)g_nf_fix_addr, (long)g_nf_fix_len,
                   G_PROT_READ | G_PROT_WRITE) == 0)
      g_nf_fixed = 1;
  }
  /* Chain to the saved (native) action with the delivered arguments. */
  if (g_saved_valid && g_saved.handler &&
      g_saved.handler != G_SIG_DFL && g_saved.handler != G_SIG_IGN) {
    if (g_saved.flags & G_SA_SIGINFO) {
      void (*fn3)(int, void*, void*) =
          (void (*)(int, void*, void*))g_saved.handler;
      g_chain_hits++;
      fn3((int)sig, info, ucv);
    } else {
      void (*fn1)(int) = (void (*)(int))g_saved.handler;
      g_chain_hits++;
      fn1((int)sig);
    }
  }
}

long g_natfault_chain_hits(void) { return (long)g_chain_hits; }

/* Arm the page and install the CHAINING handler, saving the current (native)
 * action first so the handler has something to chain to. Same bitmap as
 * g_natfault_prepare. */
long g_natfault_chain_prepare(void) {
  long r;
  g_chain_hits = 0;
  g_saved_valid = 0;
  /* Save the CURRENT action before replacing it. The host re-adopts the app's
   * native SIGSEGV action through the supported API just before this call, so
   * "current" really is that native action; saving is not order-independent,
   * because the previous round leaves a guest handler installed. */
  if (g_raw_action(11 /*SIGSEGV*/, 0, &g_saved) != 0) return -1;
  g_saved_valid = 1;
  r = g_natfault_prepare();
  if (r < 0) return r;
  if (g_action_set(11, G_KIND_CHAIN, 0, 0, 0, 0) != 0) return -2;
  return r | 4;
}

/* Call an already-built guest-callable bridge with one argument, so the host
 * can drive the bridged callback exactly the way the guest would. */
long g_call_bridge1(long fn, long a0) {
  long (*f)(long) = (long (*)(long))(unsigned long)fn;
  if (!f) return -1;
  return f(a0);
}

/* ------------------------------------------------------------------ *
 * Delivery.
 * ------------------------------------------------------------------ */

/* tgkill(getpid(), gettid(), sig): deterministic self-delivery on this thread. */
long g_raise(long sig) {
  return (long)g_sys4(SYS_tgkill, g_getpid(), g_gettid(), sig, 0);
}

/* Deliver to a specific tid in this process (the harness passes the reader
 * thread's tid). */
long g_raise_tid(long sig, long tid) {
  return (long)g_sys4(SYS_tgkill, g_getpid(), tid, sig, 0);
}

/* Install, then raise, then report hits: a compact "did the guest action
 * actually receive it" probe. Returns hits_a, or the raw install refusal when
 * the installation itself failed. */
long g_install_and_raise(long sig, long kind, long flags, long mask) {
  i64 ret = g_action_set(sig, kind, 0, 0, flags, mask);
  if (ret != 0) return -1000 + ret;
  (void)g_raise(sig);
  return (long)g_hits_a;
}

long g_action_hits_a(void) { return (long)g_hits_a; }
long g_action_hits_b(void) { return (long)g_hits_b; }
long g_action_hits_reset(void) { return (long)g_hits_reset; }
long g_action_handler_sig(void) { return (long)g_handler_sig; }
long g_action_handler_tid(void) { return (long)g_handler_tid; }
long g_action_handler_frame(void) { return (long)g_handler_frame; }

/* The raw mask the guest had while its handler was running (bit i => signal
 * i+1 blocked). Read inside the handler with rt_sigprocmask(2). */
long g_action_handler_mask(void) { return (long)g_handler_mask; }

/* The raw mask right now, same encoding. */
long g_mask_now(void) {
  u64 m = 0;
  if (g_raw_mask(0, 0, &m) != 0) return -1;
  return (long)m;
}

long g_mask_set(long mask) {
  u64 m = (u64)mask;
  return (long)g_raw_mask(2 /*SIG_SETMASK*/, &m, 0);
}

long g_mask_block(long sig) {
  u64 m = g_bit(sig);
  return (long)g_raw_mask(0 /*SIG_BLOCK*/, &m, 0);
}

long g_mask_unblock(long sig) {
  u64 m = g_bit(sig);
  return (long)g_raw_mask(1 /*SIG_UNBLOCK*/, &m, 0);
}

/* Inside-handler probes for one delivery: sets up the handler (kind A, raw
 * flags/mask), clears the records, raises, and returns a bitmap:
 *   bit 0  handler A ran
 *   bit 1  handler saw the right signal number
 *   bit 2  the raised signal was blocked while the handler ran (no SA_NODEFER)
 *   bit 3  every extra bit of the installation mask was blocked
 *          (only meaningful when `mask` is non-zero)
 *   bit 4  the caller's pre-set bit was blocked inside
 *          (only meaningful when `pre_bit` is non-zero)
 *   bit 5  only the expected bits are blocked (no stray bit): the installation
 *          mask, the automatic self-block and the caller's pre-set bit and
 *          NOTHING else
 *   bit 6  the handler ran on this thread
 *   bit 7  NOTHING of the handler's additions survived the return
 *   bit 8  the caller's own pre-call bit is still there after the return
 *          (only meaningful when `pre_bit` is non-zero)
 * `mask` is the installation mask (extra blocked bits), `pre` a mask the caller
 * set before the call. */
long g_nested_mask_probe(long sig, long flags, long mask, long pre_bit) {
  u64 after = 0;
  u64 expect;
  u64 bm = 0;
  g_hits_a = 0;
  g_handler_mask = 0;
  g_handler_sig = 0;
  if (g_action_set(sig, G_KIND_GUEST_A, 0, 0, flags, mask) != 0) return -1;
  (void)g_raise(sig);
  if (g_raw_mask(0, 0, &after) != 0) return -2;
  expect = ((u64)mask) | g_bit(sig);
  /* The pre-set bit is NOT something the handler added, so it must be allowed
   * to stay blocked inside; not counting it here would report a stray bit. */
  { u64 inside_allowed = expect | (u64)pre_bit;
  if ((g_handler_mask & ~inside_allowed) == 0) bm |= 32; }
  if (g_hits_a) bm |= 1;
  if (g_handler_sig == sig) bm |= 2;
  if (g_handler_mask & g_bit(sig)) bm |= 4;      /* auto-block (no NODEFER) */
  if (mask && (g_handler_mask & ((u64)mask)) == ((u64)mask)) bm |= 8;
  if (pre_bit && (g_handler_mask & (u64)pre_bit)) bm |= 16;
  if (g_handler_tid == g_gettid()) bm |= 64;     /* ran on this thread */
  /* bit 7: NOTHING of the handler's additions survived the return */
  if ((after & expect) == 0) bm |= 128;
  /* bit 8: the caller's own pre-call bits are all still there */
  if (pre_bit && (after & (u64)pre_bit) == (u64)pre_bit) bm |= 256;
  return (long)bm;
}

/* The same delivery shape as g_nested_mask_probe(), but with NO extra sa_mask
 * and the caller's raw flags, so one function covers:
 *   flags = 0                    -> auto-block of `sig` inside, cleared after
 *   flags = G_SA_NODEFER         -> `sig` NOT blocked inside, cleared after
 *   flags = G_SA_RESETHAND       -> handler runs once, action is gone after
 * Returns a bitmap:
 *   bit 0 handler ran
 *   bit 1 the signal was blocked while the handler ran
 *   bit 2 the signal was NOT blocked while the handler ran
 *   bit 3 the signal is unblocked again after the return
 *   bit 4 the signal's action still exists after one delivery (query ok)
 *   bit 5 the signal's action is DFL after one delivery (RESETHAND worked)
 *   bit 6 the queried logical action is a custom handler (not DFL/IGN).
 * The guest's raw rt_sigaction syscall is virtualized by the engine, so bits
 * above bit 3 describe the LOGICAL action. Only a host sigaction query can
 * inspect whether the real kernel dispatcher remains installed. */
long g_one_shot_probe(long sig, long flags) {
  struct g_sigaction q;
  u64 bm = 0;
  u64 after = 0;
  g_hits_a = 0;
  g_handler_mask = 0;
  g_memset(&q, 0, sizeof(q));
  if (g_action_set(sig, G_KIND_GUEST_A, 0, 0, flags, 0) != 0) return -1;
  (void)g_raise(sig);
  (void)g_raw_mask(0, 0, &after);
  if (g_hits_a) bm |= 1;
  if (g_handler_mask & g_bit(sig)) bm |= 2;
  if (!(g_handler_mask & g_bit(sig))) bm |= 4;
  if (!(after & g_bit(sig))) bm |= 8;
  if (g_raw_action(sig, 0, &q) == 0) {
    bm |= 16;
    if (q.handler == G_SIG_DFL || q.handler == G_SIG_IGN) bm |= 32;
    if (q.handler != G_SIG_DFL && q.handler != G_SIG_IGN && q.handler != 0)
      bm |= 64;
  }
  return (long)bm;
}

/* ------------------------------------------------------------------ *
 * Alt stack.
 * ------------------------------------------------------------------ */

long g_altstack_set(long flags) {
  struct g_stack ss;
  ss.ss_sp = (void*)g_altstack_area;
  ss.ss_flags = (int)flags;
  ss.ss_size = sizeof(g_altstack_area);
  return (long)g_sys2(SYS_sigaltstack, (i64)&ss, 0);
}

long g_altstack_disable(void) {
  struct g_stack ss;
  ss.ss_sp = 0;
  ss.ss_flags = G_SS_DISABLE;
  ss.ss_size = 0;
  return (long)g_sys2(SYS_sigaltstack, (i64)&ss, 0);
}

/* Query the guest alt stack into a caller buffer {sp, flags, size}. */
long g_altstack_get(void* out) {
  struct g_stack ss;
  i64 ret;
  g_memset(&ss, 0, sizeof(ss));
  ret = g_sys2(SYS_sigaltstack, 0, (i64)&ss);
  if (out) g_memcpy(out, &ss, sizeof(ss));
  return (long)ret;
}

long g_altstack_base(void) { return (long)(unsigned long)g_altstack_area; }
long g_altstack_size(void) { return (long)sizeof(g_altstack_area); }

/* Deliver `sig` with an ONSTACK handler installed and report a bitmap:
 *   bit 0 handler ran
 *   bit 1 the handler's frame is inside [base, base+size)
 *   bit 2 the frame is not inside the disabled-case address range
 * The host additionally asserts a local OUTSIDE the handler is not in the
 * range, from its own frame. */
long g_altstack_delivery_probe(long sig) {
  u64 bm = 0;
  u64 base = (u64)(unsigned long)g_altstack_area;
  u64 size = sizeof(g_altstack_area);
  g_hits_a = 0;
  g_handler_frame = 0;
  if (g_altstack_set(0) != 0) return -1;
  if (g_action_set(sig, G_KIND_GUEST_A, 0, 0, G_SA_ONSTACK, 0) != 0) return -2;
  (void)g_raise(sig);
  if (g_hits_a) bm |= 1;
  if (g_handler_frame >= base && g_handler_frame < base + size) bm |= 2;
  {
    volatile u64 outside = 0;
    if (!((u64)(unsigned long)&outside >= base &&
          (u64)(unsigned long)&outside < base + size))
      bm |= 4;
  }
  /* The alt stack stays installed on purpose: the host must query the SAME
   * registration the handler ran under, and only then clear it through
   * g_altstack_disable() as the owner of the cleanup step. */
  return (long)bm;
}

/* ------------------------------------------------------------------ *
 * Blocking read, for the SA_RESTART / EINTR cases.
 * ------------------------------------------------------------------ */

/* Install a 1-arg guest handler for `sig` with the caller's logical flags, make
 * sure it is unblocked, and report the raw result. */
long g_restart_install(long sig, long flags) {
  if (g_action_set(sig, G_KIND_GUEST_A, 0, 0, flags, 0) != 0) return -1;
  if (g_mask_unblock(sig) != 0) return -2;
  g_hits_a = 0;
  return 0;
}

/* Raw blocking read. Returns the byte count, or -errno. This is the syscall a
 * signal must interrupt (with SA_RESTART clear) or restart (with it set). */
long g_read_fd(long fd, void* buf, long len) {
  i64 ret = g_sys4(SYS_read, fd, (i64)buf, len, 0);
  if (ret < 0) return -(-ret); /* -errno, already negative */
  return (long)ret;
}

/* ------------------------------------------------------------------ *
 * SMC / RWX guest code (the GS=0 dynarec internal-repair case).
 * ------------------------------------------------------------------ */

/* mmap a private anonymous RWX page. Returns the address or -errno. */
long g_mmap_rwx(long size) {
  i64 ret = g_sys6(SYS_mmap, 0, size, G_PROT_READ | G_PROT_WRITE | G_PROT_EXEC,
                   G_MAP_PRIVATE | G_MAP_ANONYMOUS, -1, 0);
  if (ret < 0 && ret > -4096) return -(-ret);
  return (long)ret;
}

long g_mprotect(long addr, long size, long prot) {
  return (long)g_sys4(SYS_mprotect, addr, size, prot, 0);
}

/* Build `mov eax, imm32; ret` at `code`, call it twice: first with imm=42, then
 * after patching the immediate to 7. Returns a bitmap:
 *   bit 0  first call returned 42
 *   bit 1  second call returned 7 (the patch was observed)
 *   bit 2  the two calls used ONE code page (same address)
 * The host additionally asserts its SIGSEGV counter stayed zero, i.e. the
 * write-protection fault that auto-SMC repair takes was handled INSIDE the
 * engine and never reached a host action. */
long g_smc_probe(void) {
  unsigned char* code;
  long page = 4096;
  long (*fn)(void);
  long r1, r2;
  u64 bm = 0;

  code = (unsigned char*)g_mmap_rwx(page);
  if ((long)(unsigned long)code <= 0) return (long)(unsigned long)code;

  /* mov eax, 42 ; ret  =>  B8 2A 00 00 00 C3 */
  code[0] = 0xb8;
  code[1] = 42;
  code[2] = 0x00;
  code[3] = 0x00;
  code[4] = 0x00;
  code[5] = 0xc3;

  fn = (long (*)(void))code;
  r1 = fn();
  if (r1 == 42) bm |= 1;

  /* Patch the immediate in place: this write hits a page the dynarec
   * write-protected, so the engine must repair its own protection fault. */
  code[1] = 7;
  r2 = fn();
  if (r2 == 7) bm |= 2;
  if ((u64)(unsigned long)fn == (u64)(unsigned long)code) bm |= 4;
  return (long)bm;
}

/* ------------------------------------------------------------------ *
 * Hardware/guest fault recovery.
 * ------------------------------------------------------------------ */

/* Guest-side page-fault recovery, in two steps so the host stays in control of
 * WHEN the faulting store happens:
 *
 *   g_fault_prepare()   mmap a PROT_READ page, install a guest SIGSEGV handler
 *                       (handler_kind, or the engine action already in place
 *                       when it is < 0), arm the address the handler repairs,
 *                       and return the page address.
 *   g_fault_store()     the store that must fault exactly once; the guest
 *                       handler raw-mprotects the page writable and returns, so
 *                       box64 must resume the guest AT the faulting instruction
 *                       with no native-PC fiction.
 *
 * The host wraps the store in its own watchdog so a failure can never hang.
 */
static volatile u64 g_fault_page;

long g_fault_prepare(long handler_kind) {
  unsigned char* p;
  long page = 4096;
  p = (unsigned char*)g_sys6(SYS_mmap, 0, page, G_PROT_READ,
                             G_MAP_PRIVATE | G_MAP_ANONYMOUS, -1, 0);
  if ((long)(unsigned long)p <= 0) return (long)(unsigned long)p;
  g_fault_page = (u64)(unsigned long)p;
  g_fix_addr = (u64)(unsigned long)p;
  g_fix_len = (u64)page;
  g_hits_a = 0;
  g_mprotect_fixed = 0;
  if (handler_kind >= 0) {
    if (g_action_set(11 /*SIGSEGV*/, handler_kind, 0, 0, 0, 0) != 0) return -1;
  }
  return (long)(unsigned long)p;
}

/* The faulting store. Returns a bitmap:
 *   bit 0  the store completed (the engine resumed at the faulting store)
 *   bit 1  the guest handler ran
 *   bit 2  the handler's raw mprotect made the page writable
 *   bit 3  the value the store wrote is readable back
 */
long g_fault_store(long expect) {
  volatile unsigned char* p = (volatile unsigned char*)g_fault_page;
  u64 bm = 0;
  if (!g_fault_page) return -1;
  *p = (unsigned char)expect;
  bm |= 1;
  if (g_hits_a) bm |= 2;
  if (g_mprotect_fixed) bm |= 4;
  if (*(volatile unsigned char*)g_fault_page == (unsigned char)expect) bm |= 8;
  return (long)bm;
}

long g_fault_page_addr(void) { return (long)(unsigned long)g_fault_page; }

/* The repairing SIGSEGV handler, exposed so the host can install it through the
 * native action setter and prove a GUEST-fault delivery reaches a NATIVE action
 * (the guest/native unity direction) as well as a guest one. */
long g_pagefix_handler_addr(void) { return (long)(unsigned long)g_handler_pagefix; }

/* ------------------------------------------------------------------ *
 * SIGCHLD / default-disposition probes.
 * ------------------------------------------------------------------ */

/* Install SIGCHLD with the caller's logical flags, fork a child that exits
 * immediately, wait for it with the raw wait4(2), and report a bitmap:
 *   bit 0  fork produced a child
 *   bit 1  wait4 returned -ECHILD (the child was auto-reaped: NOCLDWAIT)
 *   bit 2  wait4 returned the child's pid (a normal reap)
 *   bit 3  the SIGCHLD handler ran
 * Used only with the flags that make the outcome deterministic on each side. */
long g_sigchld_probe(long flags) {
  u64 bm = 0;
  i64 pid;
  int status = 0;
  i64 w;
  g_hits_b = 0;
  if (g_action_set(17 /*SIGCHLD*/, G_KIND_CHLD, 0, 0, flags, 0) != 0) return -1;
  pid = g_sys0(SYS_fork);
  if (pid == 0) {
    (void)g_sys1(SYS_exit_group, 0);
  }
  if (pid < 0) return -2;
  bm |= 1;
  w = g_sys4(SYS_wait4, pid, (i64)&status, 0 /*WNOHANG clear*/, 0);
  if (w == -10 /*-ECHILD*/) bm |= 2;
  else if (w == pid) bm |= 4;
  if (g_hits_b) bm |= 8;
  return (long)bm;
}

/* Raise `sig` with the raw syscall and then sleep for a long, bounded time:
 * only ever reached by a child process whose action is DEFAULT, where the
 * delivery is fatal and the sleep is not reached at all. The sleep is the
 * watchdog: if the disposition turned out to be something non-fatal the child
 * exits 0 well before the parent's timeout, so the case fails loudly instead of
 * hanging. */
long g_raise_and_wait(long sig) {
  struct g_timespec ts;
  ts.tv_sec = 3;
  ts.tv_nsec = 0;
  (void)g_raise(sig);
  (void)g_sys2(SYS_nanosleep, (i64)&ts, 0);
  return 0;
}

/* Sleep so the host can synchronise a worker-driven delivery. */
long g_sleep_ns(long sec, long nsec) {
  struct g_timespec ts;
  ts.tv_sec = sec;
  ts.tv_nsec = nsec;
  return (long)g_sys2(SYS_nanosleep, (i64)&ts, 0);
}

/* ------------------------------------------------------------------ *
 * Identity / sanity.
 * ------------------------------------------------------------------ */

long g_self_tid(void) { return (long)g_gettid(); }
long g_self_pid(void) { return (long)g_getpid(); }

/* Always 0x5a17: proves the fixture loaded and the call path works before any
 * signal case runs. */
long g_probe_alive(void) { return 0x5a17; }
