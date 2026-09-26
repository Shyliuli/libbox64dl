/*
 * wrapper.h — the single, explicit bindgen input.
 *
 * Only our own private C shim is visible here (src/core_shim.h, which itself
 * includes only <stdint.h>). The vendored box64 headers are deliberately NOT
 * included, so no box64 private type or function can leak into the Rust
 * business code. pt_mutex keeps its own three-line extern block because it is a
 * hand-written pthread primitive.
 */
/* bindgen ignores declarations it cannot spell cleanly; the hidden-visibility
 * attribute is irrelevant to the generated Rust and is cleared for parsing. */
#define BOX64DL_CORE_API
#include "../src/core_shim.h"
