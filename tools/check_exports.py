#!/usr/bin/env python3
"""Gate the libbox64dl DSO symbol surface.

The shipped export table is the engine DSO (``build/lib/libbox64dl.so``).  The
gate is self-contained: it never looks at winedl, Node or any sibling
repository, so an engine-only build can run it.

The table has exactly three dynamic classes, and this script checks all of them:

  1. the public embed ABI (``box64_*``), which downstream consumers link;
  2. the runtime-trampoline primitive (``box64dl_thunk_*``,
     ``include/box64dl_thunk.h``): a guest->host callable thunk can only be
     built by the engine that owns the registry, so a DSO consumer links these;
  3. the engine-internal wrapped-library entry points (``my*``), which box64's
     own librarian resolves with ``dlsym(my_context->box64lib, "my_"+name)``
     (vendor ``librarian/library.c``) and which therefore have to be dynamic
     even though they are not API.  This is not a "four box64 exports" surface:
     the ``my*`` class is thousands of wrapper symbols, including the
     ``my2_``/``my32_`` altmy variants.

Everything else must be local: the Rust runtime (``_ZN``/``_R`` mangled), the
``box64dl_core_*`` shim and all remaining box64 internals (including the
``box64dl_thunk_code`` asm template).  A dynamic symbol that is neither
``box64_*``, ``box64dl_thunk_*`` nor ``my*`` is a hard failure, which is what
keeps "Rust and the shim are hidden" checkable with nm.

``--inject-symbol`` is test-only: the negative gate proves each forbidden class
is enforced by adding one name to the observed set and requiring a failure.
"""
from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

# Retired forward-call entries (historical register-file / marshaled / plan /
# fixed-arity library symbols) and the retired ready alias.
RETIRED = {
    "box64_call_regs",
    "box64_call_marshaled",
    "box64_call_plan",
    "box64_lib_ready",
    *(f"box64_call{i}" for i in range(7)),
}

# Any Node-side name leaking into the engine is a reverse-dependency regression.
NODE_FORBIDDEN_PREFIXES = ("winedl_", "uninode_", "napi_", "my_napi_", "libnode")

# Public embed ABI: unified forward-call entries + Rust-owned lifecycle +
# loader/callback/bridge accessors.
REQUIRED_PUBLIC = (
    "box64_call_simple",
    "box64_call_desc",
    "box64_lib_init",
    "box64_lib_fini",
    "box64_is_ready",
    "box64_ensure_emu",
    "box64_engine_phase",
    "box64_dlopen",
    "box64_dlsym",
    "box64_dlclose",
    "box64_dlerror",
    "box64_callback",
    "box64_callback_with",
    "box64_bridge_args",
    "box64_bridge_ret",
    # Explicit native-module seam (include/libbox64_native_module.h). The
    # provider links the registration entry and the wrapped path consults the
    # lookups and the signature->wrapper table; all of them must stay dynamic
    # or the seam cannot be used downstream. box64_native_function() is the
    # public form of the bridge-address query the ELF provider's callback
    # rewriting uses.
    "box64_register_native_module",
    "box64_native_module_for_soname",
    "box64_native_module_for_name",
    "box64_native_module_wrapper",
    "box64_native_function",
)

# Engine-internal wrapper symbols the runtime resolves through dlsym().  These
# are exactly the wrapped libc entries the real DT_NEEDED-libc guest consumer
# exercises; a DSO that does not export them cannot run that guest.
REQUIRED_WRAPPERS = (
    "my_printf",
    "my_fopen",
    "my_bsearch",
    "my_qsort",
    "my___cxa_atexit",
)

# Runtime-trampoline primitive (include/box64dl_thunk.h). The engine owns the
# registry, so a DSO consumer (uninode's PE bridge) cannot build a guest->host
# thunk itself and has to link these. The asm template data symbols
# (box64dl_thunk_code / _end) are deliberately NOT part of this class.
REQUIRED_THUNK_API = (
    "box64dl_thunk_get",
    "box64dl_thunk_peek",
    "box64dl_thunk_data0",
    "box64dl_thunk_release",
    "box64dl_thunk_release_range",
    "box64dl_thunk_count",
)

# The my* class is a table, not a handful of names; a token export must fail.
MIN_WRAPPER_SYMBOLS = 1000
MIN_ALT_PREFIX_SYMBOLS = 1  # my2_/my32_ altmy variants


def symbols(path: Path) -> set[str]:
    cmd = ["nm", "-D", "--defined-only", str(path)]
    try:
        out = subprocess.check_output(cmd, text=True, stderr=subprocess.STDOUT)
    except (OSError, subprocess.CalledProcessError) as exc:
        print(f"symbol-gate: cannot inspect {path}: {exc}", file=sys.stderr)
        raise SystemExit(2)
    found: set[str] = set()
    for line in out.splitlines():
        fields = line.split()
        if fields:
            found.add(fields[-1])
    return found


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--box64", type=Path, required=True)
    ap.add_argument("--inject-symbol", action="append", default=[],
                    help="negative-test only: add a fake symbol to the observed set")
    ns = ap.parse_args()

    box = symbols(ns.box64)
    for injected in ns.inject_symbol:
        box.add(injected)

    public = {n for n in box if n.startswith("box64_")}
    trampolines = {n for n in box if n in REQUIRED_THUNK_API}
    wrappers = {n for n in box if n.startswith("my")}
    # Anything else is a leaked internal (Rust mangling, box64dl_core_* shim,
    # the thunk asm template, or a box64 internal the version script failed to
    # localise).
    leaked = box - public - trampolines - wrappers

    errors: list[str] = []
    for name in sorted(RETIRED & box):
        errors.append(f"retired libbox64dl symbol exported: {name}")
    for name in sorted(box):
        if any(name.startswith(p) for p in NODE_FORBIDDEN_PREFIXES):
            errors.append(f"Node/foreign symbol exported by the engine: {name}")
    for name in sorted(leaked):
        errors.append(
            "internal symbol exported by the engine (expected only box64_*, "
            f"box64dl_thunk_* and my*): {name}"
        )
    for name in REQUIRED_PUBLIC:
        if name not in public:
            errors.append(f"missing public libbox64dl symbol: {name}")
    for name in REQUIRED_THUNK_API:
        if name not in trampolines:
            errors.append(f"missing runtime-trampoline symbol: {name}")
    for name in REQUIRED_WRAPPERS:
        if name not in wrappers:
            errors.append(f"missing internal wrapper dynamic symbol: {name}")
    if len(wrappers) < MIN_WRAPPER_SYMBOLS:
        errors.append(
            f"only {len(wrappers)} my* wrapper symbols exported; the wrapped "
            f"library table needs at least {MIN_WRAPPER_SYMBOLS}"
        )
    alt = sum(1 for n in wrappers if n.startswith("my2_") or n.startswith("my32_"))
    if alt < MIN_ALT_PREFIX_SYMBOLS:
        errors.append("no my2_/my32_ altmy wrapper dynamic symbols exported")

    if errors:
        for error in errors:
            print(f"symbol-gate: FAIL: {error}", file=sys.stderr)
        return 1
    print(
        "symbol-gate: PASS "
        f"({len(public)} public box64_* ABI + {len(trampolines)} "
        f"box64dl_thunk_* trampolines + {len(wrappers)} internal my* "
        "wrapper dynamic symbols; no retired/Node/internal symbol exported)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
