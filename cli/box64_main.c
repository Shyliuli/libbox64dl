/*
 * box64 — the packaged CLI entry of the embedded engine.
 *
 * The LoongArch executable an app tree ships as runtime/bin/box64: the same
 * initialize() + emulate() startup as upstream's main.c, linked over this
 * project's static libbox64.a + Rust facade. It behaves exactly like a
 * system box64 (guest argv/env, box64path derived from its own argv[0]),
 * which is what winedl needs it for: wine's companion children
 * (wineserver, wineboot, CreateProcess) are re-exec'd through it, both by
 * standalone spawns and by the embedded engine's box64path. The embedded
 * patches are no-ops on this path (0003's host-route never fires without
 * foreign host threads; 0002 only silences the default log).
 */
#include "box64_main.h"

int main(int argc, const char** argv, char** env) {
    return box64_main(argc, argv, env);
}
