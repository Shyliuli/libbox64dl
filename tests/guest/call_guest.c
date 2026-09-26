/*
 * x86_64 guest fixture for tests/test_call_arity.c.
 *
 * Built freestanding (-nostdlib -fPIC -shared) by the x86_64 cross toolchain:
 * no guest libc, no undefined symbols, so box64 can dlopen it on any host.
 * Every function is trivial and self-contained so the test exercises the
 * engine's call path, not work done inside the guest.
 */

/* DT_INIT_ARRAY marker: box64 must run the constructor on dlopen. */
static long g_state = 0x1122334455667788L;

__attribute__((constructor)) static void guest_ctor(void) {
  g_state = 0x5a5a5a5a5a5a5a5aL;
}

/* 0 args, non-zero result: distinguishes "ran" from a refusal (which is 0). */
long g_zero(void) { return 0x1234; }

/* 0 args, legitimate zero: proves 0 means "ran" when the error slot is clear. */
long g_retzero(void) { return 0; }

long g_ctor_marker(void) { return g_state; }

/* Pointer-width identity, for high-bit and negative arguments. */
long g_id(long a) { return a; }

long g_add2(long a, long b) { return a + b; }

long g_sum6(long a, long b, long c, long d, long e, long f) {
  return a + b + c + d + e + f;
}

/* Four stack arguments after the six register ones (SysV64). */
long g_sum10(long a, long b, long c, long d, long e, long f, long g, long h,
             long i, long j) {
  return a + b + c + d + e + f + g + h + i + j;
}

/* 32-bit sign handling: the callee sees the low 32 bits as int. */
long g_neg32(int x) { return -(long)x; }

/* 64-bit sign handling. */
long g_neg64(long x) { return -x; }

long g_ptr_long(const long* p) { return *p; }

/* Calls a host-supplied function pointer: the bridge re-enters the engine
 * through the unified scalar call entry, which is the reentrancy check. */
long g_invoke(long (*f)(long), long x) { return f(x) + 1; }

/* Recursion: the guest genuinely uses its stack, and the engine must restore
 * the pre-call SP afterwards. */
long g_deep(long n) { return n <= 0 ? 0 : g_deep(n - 1) + 1; }
