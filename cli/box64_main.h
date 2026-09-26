/*
 * box64_main.h — the packaged CLI entry declaration.
 *
 * Deliberately separate from include/libbox64.h: `box64_main` is a CLI/DSO
 * entry that drives box64's own initialize()/emulate() startup, not part of the
 * embeddable library ABI. The CLI translation unit (cli/box64_main.c) must not
 * include the public library header.
 */
#ifndef BOX64_CLI_MAIN_H
#define BOX64_CLI_MAIN_H

#if defined(__GNUC__)
#define BOX64_CLI_API __attribute__((visibility("default")))
#else
#define BOX64_CLI_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

BOX64_CLI_API int box64_main(int argc, const char** argv, char** env);

#ifdef __cplusplus
}
#endif

#endif /* BOX64_CLI_MAIN_H */
