/*
 * Struct / float guest fixture for tests/test_call_plan.c.
 *
 * Every `target_*` function has an `oracle_*` wrapper that the SAME x86_64
 * compiler generated. The wrapper takes the struct's scalar fields as ordinary
 * separate arguments, builds the struct and calls the target, so calling the
 * oracle and the target through the engine seam and requiring equal results
 * makes the compiler the authority on how the aggregate crosses the ABI. The
 * host test never has to trust a hand-derived placement rule (the earlier
 * `struct { float a, b, c; }` = "xmm0 low 12 bytes" reading was wrong: it is
 * two SSE eightbytes, xmm0 = {a,b} and xmm1 = {c}).
 *
 * Built freestanding with the x86_64 cross toolchain, like call_guest.c.
 */

typedef unsigned long u64;

struct s3f {
  float a, b, c;
};
struct s2d {
  double a, b;
};
struct sfd {
  float a;
  double b;
};
struct fffp {
  float a, b, c;
  void* p;
};
struct big {
  long a, b, c;
};
struct big5 {
  long a, b, c, d, e;
};

/* --- s3f: two SSE eightbytes (xmm0={a,b}, xmm1={c}) ------------------- */
float target_s3f(struct s3f s) {
  return s.a + s.b * 10.0f + s.c * 100.0f;
}
float oracle_s3f(float a, float b, float c) {
  struct s3f s = {a, b, c};
  return target_s3f(s);
}

/* --- s2d: two SSE eightbytes, one register each (xmm0 = {a}, xmm1 = {b});
 * SSEUP only packs vector types, not ordinary double/float members -------- */
double target_s2d(struct s2d s) { return s.a + s.b * 1000.0; }
double oracle_s2d(double a, double b) {
  struct s2d s = {a, b};
  return target_s2d(s);
}

/* --- sfd: xmm0 low = a, xmm1 low = b -------------------------------- */
double target_sfd(struct sfd s) { return (double)s.a + s.b * 1000.0; }
double oracle_sfd(float a, double b) {
  struct sfd s = {a, b};
  return target_sfd(s);
}

/* --- i(fffp)p: rdi=a0; xmm0={a,b}; xmm1={c}; rsi=s.p; rdx=p --------- */
double target_i_fffp_p(long a0, struct fffp s, void* p) {
  return (double)a0 + (double)s.a + (double)s.b * 10.0 +
         (double)s.c * 100.0 + (double)(long)s.p + (double)(long)p * 7.0;
}
double oracle_i_fffp_p(long a0, float a, float b, float c, void* sp, void* p) {
  struct fffp s = {a, b, c, sp};
  return target_i_fffp_p(a0, s, p);
}

/* --- big: 24 bytes, MEMORY class -> stack (24 is not 16-aligned) ----- */
long target_big(long x, struct big b) { return x + b.a + b.b * 10 + b.c * 100; }
long oracle_big(long x, long a, long b, long c) {
  struct big s = {a, b, c};
  return target_big(x, s);
}

/* --- big5: 40 bytes on the stack (40 is not 16-aligned) -------------- */
long target_big5(long x, struct big5 b) {
  return x + b.a + b.b + b.c + b.d + b.e;
}
long oracle_big5(long x, long a, long b, long c, long d, long e) {
  struct big5 s = {a, b, c, d, e};
  return target_big5(x, s);
}

/* --- ret_big: a 24-byte return, so SysV64 returns it in MEMORY ---------
 * `target_ret_big` takes its single declared `long` in rsi (rdi is the hidden
 * destination pointer), writes the aggregate through that pointer and returns
 * the pointer in rax. The three fields are full-width, byte-order-sensitive
 * functions of x, so a host that only reads the returned bytes can still tell
 * the fields and their endianness apart.
 *
 * The oracle cannot return the struct without re-entering the same sret path,
 * so it exposes each field as an ordinary scalar `long` return computed by the
 * same compiler; comparing the decoded aggregate bytes against it makes the
 * compiler, not a hand-derived placement rule, the authority. */
struct ret_big {
  long a, b, c;
};

struct ret_big target_ret_big(long x) {
  struct ret_big r;
  r.a = x;
  r.b = ~x;
  r.c = -x;
  return r;
}

long oracle_ret_big(long x, long field) {
  switch (field) {
    case 0:
      return x;
    case 1:
      return ~x;
    default:
      return -x;
  }
}
