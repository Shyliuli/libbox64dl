#ifndef BOX64DL_H_
#define BOX64DL_H_

#include <stdint.h>
#include <stdlib.h>

#include "test.h"

//TODO: 我们要有更好的头文件绑定...

#define BOX64DL_RTLD_NOW 2
#define BOX64DL_RTLD_GLOBAL 0x100
#define FAIL(...) do { eprintf(__VA_ARGS__); exit(-1); } while (0)

typedef enum {
    PLAN_OK = 0,
    PLAN_NOT_SUPPORT = -1,
    PLAN_INVALID_ARGUMENT = -2,
} AbiPlanError;

typedef enum {
    REG_NONE,
    REG_RDI,
    REG_RSI,
    REG_RDX,
    REG_RCX,
    REG_R8,
    REG_R9,
    REG_RAX,
} Register;

typedef struct {
    Register args[6];
    Register result;
} Plan;

typedef enum {
    C_INT,
    C_UNSUPPORTED,
    C_VOID,
} CType;

extern void box64dl_init(void);
extern AbiPlanError box64dl_get_plan(const CType* args, int len, CType result, Plan* plan);
extern void* box64dl_dlopen(const char* path, int flags);
extern void* box64dl_sym(void* handle, const char* name);
extern int box64dl_close(void* handle);
extern AbiPlanError box64dl_call(void* handle, const Plan* plan, const uint64_t* args, void* ret);

#endif
