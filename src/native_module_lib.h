/*
 * native_module_lib.h — engine-side construction of a runtime wrapped library
 * from a registered native-module descriptor (include/libbox64_native_module.h).
 *
 * Vendored librarian/library.c calls the single entry below from
 * initWrappedLib(); the implementation lives in native_module_lib.c, which is
 * the only place that needs box64's private library/bridge types. Keeping the
 * logic out of the vendored file keeps the vendor patch to one call site.
 */
#ifndef BOX64DL_NATIVE_MODULE_LIB_H
#define BOX64DL_NATIVE_MODULE_LIB_H

struct library_s;
struct box64context_s;

/*
 * Tri-state result for the guest soname lib->name:
 *
 *    0  no registered descriptor claims the soname: the caller falls back to
 *       the static wrapped table exactly as before.
 *    1  a descriptor claims the soname and the runtime wrapped library was
 *       built; the caller is done.
 *   -1  a descriptor claims the soname but the load failed closed (host
 *       dlopen failure, OOM, linkmap failure, or the claimed descriptor
 *       vanished). The caller MUST abort the load: it must not continue to
 *       initEmulatedLib(), because a same-named x86_64 guest ELF in the search
 *       path must not be loaded and its constructors must not run; and it must
 *       not leave a half-initialised LIB_WRAPPED behind. This function undoes
 *       every allocation and library mutation it made before returning -1.
 */
int box64dl_native_module_init_wrapped(struct library_s* lib,
                                       struct box64context_s* context);

#endif /* BOX64DL_NATIVE_MODULE_LIB_H */
