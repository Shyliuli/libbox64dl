#include <stdint.h>
#include <test.h>

enum { CTYPE_INT = 0, CTYPE_FLOAT = 1, CTYPE_VOID = 2 };
typedef struct CType {
    uint32_t tag;
    union {
        uintptr_t len;
        struct { struct CType *ptr; uintptr_t len; } agg;
    };
} CType;
typedef struct { CType *args; uint32_t args_len; CType ret; } CFunc;

char *box64dl_gen_stub(CFunc args, const char *func_name);

static CType c_int(uintptr_t n) { CType t = { .tag = CTYPE_INT, .len = n }; return t; }
static CType c_flt(uintptr_t n) { CType t = { .tag = CTYPE_FLOAT, .len = n }; return t; }

static void emit(const char *sig, const char *ret_decl, CFunc f, const char *name) {
    char *body = box64dl_gen_stub(f, name);
    if (!body) FAIL("box64dl_gen_stub failed for %s\n", name);
    printf("%s {\n%s\n%sreturn __ret;\n}\n", sig, ret_decl, body);
    free(body);
}

int main(void) {
    CType add_t[2] = { c_int(4), c_int(4) };
    CFunc add_f = { .args = add_t, .args_len = 2, .ret = c_int(4) };
    emit("static int add(int __arg0, int __arg1)", "int __ret = 0;", add_f, "add");

    CType twice_t[1] = { c_flt(8) };
    CFunc twice_f = { .args = twice_t, .args_len = 1, .ret = c_flt(8) };
    emit("static double twice(double __arg0)", "double __ret = 0;", twice_f, "twice");
    return 0;
}
