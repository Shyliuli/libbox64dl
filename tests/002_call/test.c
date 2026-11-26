#include <stdint.h>
#include <string.h>
#include <test.h>

// 与 abi_plan.h 布局一致
enum { CTYPE_INT = 0, CTYPE_FLOAT = 1, CTYPE_VOID = 2 };
typedef struct CType {
    uint32_t tag;
    union {
        uintptr_t len;
        struct { struct CType *ptr; uintptr_t len; } agg;
    };
} CType;
typedef struct { CType *args; uint32_t args_len; CType ret; } CFunc;
typedef struct { uint64_t lo, hi; } Xmm;

enum { REG_RAX = 0, REG_RCX = 1, REG_RDX = 2, REG_RSP = 4, REG_RSI = 6, REG_RDI = 7, REG_R8 = 8, REG_R9 = 9 };

int box64dl_init(void);
void *box64dl_dlopen(const char *path, int flags);
void *box64dl_sym(void *handle, const char *name);
int box64dl_close(void *handle);
int box64dl_call_runtime(void *func, CFunc args, void *args_val, void *ret_val);
int box64dl_call(void *func);
uint64_t *box64dl_get_thread_reg(void);
Xmm *box64dl_get_thread_reg_xmm(void);

static CType c_int(uintptr_t n) { CType t = { .tag = CTYPE_INT, .len = n }; return t; }
static CType c_flt(uintptr_t n) { CType t = { .tag = CTYPE_FLOAT, .len = n }; return t; }
static uint64_t f64_bits(double d) { uint64_t u; memcpy(&u, &d, 8); return u; }
static double bits_f64(uint64_t u) { double d; memcpy(&d, &u, 8); return d; }

int main(void) {
    if (box64dl_init() != 0) FAIL("box64dl_init failed\n");
    void *h = box64dl_dlopen("./libguest.so", BOX64DL_RTLD_NOW);
    if (!h) FAIL("box64dl_dlopen failed\n");
    void *fn_add2 = box64dl_sym(h, "add2");
    void *fn_mix8 = box64dl_sym(h, "mix8");
    if (!fn_add2 || !fn_mix8) FAIL("box64dl_sym failed\n");

    // 1. box64dl_call_runtime
    CType add2_t[2] = { c_int(8), c_int(8) };
    CFunc add2_sig = { .args = add2_t, .args_len = 2, .ret = c_int(8) };
    long a = 13, b = 29, r_add = 0;
    void *add2_v[2] = { &a, &b };
    if (box64dl_call_runtime(fn_add2, add2_sig, add2_v, &r_add) != 0 || r_add != 42)
        FAIL("call_runtime add2 got %ld, want 42\n", r_add);

    CType mix8_t[10] = {
        c_int(8), c_int(8), c_int(8), c_int(8), c_int(8), c_int(8), c_int(8), c_int(8),
        c_flt(8), c_flt(8),
    };
    CFunc mix8_sig = { .args = mix8_t, .args_len = 10, .ret = c_flt(8) };
    long i0 = 1, i1 = 2, i2 = 3, i3 = 4, i4 = 5, i5 = 6, i6 = 7, i7 = 8;
    double d0 = 0.25, d1 = 0.5, r_mix = 0;
    void *mix8_v[10] = { &i0, &i1, &i2, &i3, &i4, &i5, &i6, &i7, &d0, &d1 };
    if (box64dl_call_runtime(fn_mix8, mix8_sig, mix8_v, &r_mix) != 0 || r_mix != 36.75)
        FAIL("call_runtime mix8 got %f, want 36.75\n", r_mix);

    // 2. box64dl_call (手写寄存器与栈)
    uint64_t *regs = box64dl_get_thread_reg();
    Xmm *xmms = box64dl_get_thread_reg_xmm();
    if (!regs || !xmms) FAIL("get_thread_reg failed\n");

    regs[REG_RDI] = 100;
    regs[REG_RSI] = 23;
    if (box64dl_call(fn_add2) != 0 || (long)regs[REG_RAX] != 123)
        FAIL("box64dl_call add2 got %ld, want 123\n", (long)regs[REG_RAX]);

    uint64_t old_sp = regs[REG_RSP];
    uint64_t sp = (old_sp - 16) & ~15ULL;
    regs[REG_RSP] = sp;
    regs[REG_RDI] = 1; regs[REG_RSI] = 2; regs[REG_RDX] = 3; regs[REG_RCX] = 4;
    regs[REG_R8] = 5;  regs[REG_R9] = 6;
    ((long *)sp)[0] = 7; ((long *)sp)[1] = 8;
    xmms[0] = (Xmm){ f64_bits(1.25), 0 };
    xmms[1] = (Xmm){ f64_bits(2.5), 0 };
    if (box64dl_call(fn_mix8) != 0) FAIL("box64dl_call mix8 failed\n");
    regs[REG_RSP] = old_sp;
    if (bits_f64(xmms[0].lo) != 39.75)
        FAIL("box64dl_call mix8 got %f, want 39.75\n", bits_f64(xmms[0].lo));

    box64dl_close(h);
    return 0;
}
