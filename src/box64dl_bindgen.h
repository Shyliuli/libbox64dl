#ifndef BOX64DL_BINDGEN_H_
#define BOX64DL_BINDGEN_H_

#include <stddef.h>
#include <stdint.h>
#include "box64context.h"
#include "os.h"
#include "custommem.h"
#include "x64emu.h"
#include "x64tls.h"
#include "env.h"
#include "sysinfo.h"

extern char **environ;
int init_auxval(int argc, const char **argv, char **env);
void init_guestalloc(void);
void *my_dlopen(x64emu_t *emu, const char *filename, int flag);
void *my_dlsym(x64emu_t *emu, void *handle, const char *symbol);
int my_dlclose(x64emu_t *emu, void *handle);
void SetRDI(x64emu_t *emu, uint64_t value);
void SetRSI(x64emu_t *emu, uint64_t value);
void SetRDX(x64emu_t *emu, uint64_t value);
void SetRCX(x64emu_t *emu, uint64_t value);
void SetRAX(x64emu_t *emu, uint64_t value);
void box64_set_r8(x64emu_t *emu, uint64_t value);
void box64_set_r9(x64emu_t *emu, uint64_t value);
void box64_set_xmm(x64emu_t *emu, int index, uint64_t value);
uint64_t box64_get_xmm(x64emu_t *emu, int index);
void box64_stack_push(x64emu_t *emu, size_t size);
void box64_stack_pop(x64emu_t *emu, size_t size);
void box64_stack_write(x64emu_t *emu, size_t offset, const void *src, size_t size);
void box64_dynacall(x64emu_t *emu, void *fn);
uint64_t GetRAX(x64emu_t *emu);
void DynaCall(x64emu_t *emu, uintptr_t addr, int no_alt);
void addLibPaths(box64context_t* context);
void LoadLDPath(box64context_t* context);

#endif
