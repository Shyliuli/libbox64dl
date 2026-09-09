#include "core.h"
#include "libbox64.h"

BOX64_API int box64_main(int argc, const char** argv, char** env) {
    x64emu_t* emu = NULL;
    elfheader_t* elf = NULL;
    if (initialize(argc, argv, env, &emu, &elf, 1)) return -1;
    return emulate(emu, elf);
}
