/*
 * Second, distinct x86_64 guest image for tests/test_signal_library.c.
 *
 * It is the same real-signal fixture as tests/guest/sig_guest.c, compiled under
 * different exported names so the per-library policy test has two independent
 * guest libraries. Keeping one behavioral source avoids drift between them; the
 * only difference is the symbol names, which is exactly what "same function,
 * different name" means here.
 */
#define g_sig_install g_sig_install2
#define g_sig_fire g_sig_fire2
#define g_sig_hits g_sig_hits2
#define g_sig_change_then_fire g_sig_change_then_fire2
#define g_sig_invoke g_sig_invoke2

#include "sig_guest.c"
