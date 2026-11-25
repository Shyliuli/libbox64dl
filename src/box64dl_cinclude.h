/*用于c! 需要获取结构体内部字段*/
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include "auxval.h"
#include "box64context.h"
#include "os.h"
#include "custommem.h"
#include "x64emu.h"
#include "x64tls.h"
#include "box64cpu.h"
#include "box64cpu_util.h"
#include "debug.h"
#include "env.h"
#include "guestalloc.h"
#include "sysinfo.h"
#include "x64emu_private.h"

extern char **environ;
void *my_dlopen(x64emu_t *emu, const char *filename, int flag);
void *my_dlsym(x64emu_t *emu, void *handle, const char *symbol);
int my_dlclose(x64emu_t *emu, void *handle);
void box64_set_r8(x64emu_t *emu, uint64_t value);
void box64_set_r9(x64emu_t *emu, uint64_t value);
void box64_set_xmm(x64emu_t *emu, int index, uint64_t value);
uint64_t box64_get_xmm(x64emu_t *emu, int index);
void box64_stack_push(x64emu_t *emu, size_t size);
void box64_stack_pop(x64emu_t *emu, size_t size);
void box64_stack_write(x64emu_t *emu, size_t offset, const void *src, size_t size);
void box64_dynacall(x64emu_t *emu, void *fn);
void addLibPaths(box64context_t* context);
void LoadLDPath(box64context_t* context);