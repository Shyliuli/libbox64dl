/*
 * x86_64 guest fixture for tests/test_guest_signal.c.
 *
 * A real, guest-installed NON-fatal signal handler. The handler perturbs a dead
 * caller-saved register in its own ucontext (RIP left untouched), which makes
 * box64's my_sigactionhandler_oldcode detect the context change and resume the
 * emulator through siglongjmp(emu->jmpbuf) — the same signals.c site the FAST
 * mask policy changes. This path does NOT go through box64dl_signal_route (that
 * is only for the four fatal signals), so it is reachable on a plain Linux-x64
 * guest thread whose GS base is 0. No TEB, no fake segment base.
 *
 * Deliberately no resume label / &&label: at -O1 GCC folded that onto an earlier
 * block (verified with objdump), which would re-run tgkill forever. Perturbing
 * R11 (clobbered by every syscall, never live across one) keeps RIP unchanged,
 * so the emulator resumes right after the syscall and g_sig_fire returns
 * normally.
 *
 * Built freestanding (-nostdlib -fPIC -shared) by the x86_64 cross toolchain;
 * every syscall is issued with the raw `syscall` instruction, so the guest has
 * no libc dependency.
 */
#define _GNU_SOURCE
#include <signal.h>
#include <stdint.h>
#include <ucontext.h>

/* Kernel ABI of struct sigaction as used by rt_sigaction(2):
 * handler, flags, restorer, mask (the mask is 8 bytes for the syscall). */
struct guest_sigaction {
  void (*handler)(int, void*, void*);
  unsigned long flags;
  void (*restorer)(void);
  unsigned long mask;
};

static long g_sys4(long n, long a, long b, long c, long d) {
  /* x86_64 syscall ABI: rdi, rsi, rdx, r10, r8, r9. The 4th argument (d) goes
   * in r10; r8 is the 5th and is not used here. */
  register long r10 __asm__("r10") = d;
  long ret;
  __asm__ volatile("syscall"
                   : "=a"(ret)
                   : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10)
                   : "rcx", "r11", "memory");
  return ret;
}

static volatile long g_marker;
static volatile long g_hits;

/* Perturb R11 in the delivered context. box64 compares the guest ucontext after
 * the handler returns and, on any change, mctx2emu + siglongjmp; RIP is
 * unchanged, so the emulator resumes right after the signal-raising syscall.
 * R11 is clobbered by every syscall, so no live value is disturbed. */
static void g_handler(int sig, void* info, void* ucv) {
  (void)sig;
  (void)info;
  ucontext_t* uc = (ucontext_t*)ucv;
  g_hits++;
  uc->uc_mcontext.gregs[REG_R11] ^= 0x5a5a;
}

static struct guest_sigaction g_sa;

/* Install the handler for SIGUSR1. Returns the raw syscall result (0 on ok).
 *
 * sa_mask is NON-empty (SIGALRM) and SA_NODEFER is NOT set, so entering the
 * handler adds both SIGALRM (from sa_mask) and SIGUSR1 (kernel auto-block) to
 * the mask. The host test then asserts those extra bits do not leak after the
 * signal-driven resume: COMPAT relies on siglongjmp restoring the setjmp-time
 * mask, FAST on the ucontext (pre-delivery) mask. */
long g_sig_install(void) {
  g_sa.handler = g_handler;
  g_sa.flags = SA_SIGINFO; /* deliberately no SA_NODEFER */
  g_sa.restorer = 0;
  g_sa.mask = 1UL << (SIGALRM - 1); /* non-empty sa_mask */
  return g_sys4(13 /*rt_sigaction*/, SIGUSR1, (long)&g_sa, 0, 8);
}

/* Raise SIGUSR1 at *this* thread (tgkill) so delivery is deterministic and a
 * worker thread's signal lands on its own emu. The handler perturbs the
 * context, box64 siglongjmps and the emulator resumes after the syscall, so
 * this returns marker + number of handler hits. */
long g_sig_fire(long marker) {
  long pid = g_sys4(39 /*getpid*/, 0, 0, 0, 0);
  long tid = g_sys4(186 /*gettid*/, 0, 0, 0, 0);
  g_marker = marker;
  g_sys4(234 /*tgkill*/, pid, tid, SIGUSR1, 0);
  return g_marker + g_hits;
}

/* Diagnostics for the host test. */
long g_sig_hits(void) { return g_hits; }

/* Mask-change resume probe (patches/0005): the guest changes its own signal
 * mask with the RAW rt_sigprocmask syscall, then raises SIGUSR1. The handler
 * perturbs R11 so box64 resumes with siglongjmp. Because the change happens
 * between EmuRun entry (M0) and the signal (M1), COMPAT must put M0 back while
 * FAST keeps M1. `m1` is the 8-byte kernel sigset to set. */
long g_sig_change_then_fire(long marker, unsigned long m1) {
  long pid = g_sys4(39 /*getpid*/, 0, 0, 0, 0);
  long tid = g_sys4(186 /*gettid*/, 0, 0, 0, 0);
  unsigned long mask = m1;
  g_marker = marker;
  g_sys4(14 /*rt_sigprocmask*/, 2 /*SIG_SETMASK*/, (long)&mask, 0, 8);
  g_sys4(234 /*tgkill*/, pid, tid, SIGUSR1, 0);
  return g_marker + g_hits;
}

/* Nested probe: the host callback re-enters the engine through the unified scalar call entry and
 * fires the signal while the outer guest call is still on the stack, so the
 * siglongjmp targets the outer EmuRun jmpbuf (nested EmuRun reuses it). */
long g_sig_invoke(long (*f)(long), long x) { return f(x); }
