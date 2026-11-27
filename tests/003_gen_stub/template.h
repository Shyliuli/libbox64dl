#include <stdint.h>
#include <string.h>
#include <test.h>

typedef uint64_t u64;
typedef struct { uint64_t lo, hi; } Xmm;
typedef struct { uint64_t *regs; Xmm *xmm; } Box64State;
enum { RSP = 4, RDI = 7 };

static inline u64 align_down(u64 x, u64 a) { return x & ~(a - 1); }

int box64dl_init(void);
void *box64dl_dlopen(const char *path, int flags);
void *box64dl_sym(void *handle, const char *name);
int box64dl_call(void *func);
uint64_t *box64dl_get_thread_reg(void);
Xmm *box64dl_get_thread_reg_xmm(void);

static _Thread_local Box64State box64_state;
static void *__add_sym;
static void *__twice_sym;

static void stub_init(void) {
    if (box64dl_init() != 0) FAIL("box64dl_init failed\n");
    void *h = box64dl_dlopen("./libguest.so", BOX64DL_RTLD_NOW);
    if (!h) FAIL("box64dl_dlopen failed\n");
    __add_sym = box64dl_sym(h, "add");
    __twice_sym = box64dl_sym(h, "twice");
    if (!__add_sym || !__twice_sym) FAIL("box64dl_sym failed\n");
    box64_state.regs = box64dl_get_thread_reg();
    box64_state.xmm = box64dl_get_thread_reg_xmm();
}
