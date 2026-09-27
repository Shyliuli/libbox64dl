# vendor/box64 patch queue

The engine is the **upstream** [box64](https://github.com/ptitSeb/box64)
repository, checked out as the git submodule `vendor/box64` at tag `v0.4.2`
(commit `7eeb5016493dab4e143d53da50dd47bfb44a9509`), plus the ordered patches in
this directory. There is no local vendor fork and no hand-edited copy of box64 in
this repository: the submodule stays re-checkoutable, `make patch` applies the
queue into its worktree and `make graft` links this repository's sources into it.

- `box64.lock` — the version lock: submodule commit, upstream tag and every
  patch file's sha256.
- `0001..0018-*.patch` — the queue, applied in filename order by `make patch`
  and reversed by `make unpatch`.
- `../tools/verify_box64_patch.sh` — verifier (`--check` / `--write`).

```sh
tools/verify_box64_patch.sh --check   # submodule commit + hashes + pristine apply + drift
tools/verify_box64_patch.sh --write   # re-cut the lock after a deliberate change
make patch                            # apply the queue to vendor/box64
```

## What each patch is

| patch | scope |
| --- | --- |
| `0001-export-openFTrace.patch` | export `openFTrace` for the Rust bootstrap |
| `0002-silent-log-default-unless-env.patch` | default-silent logging unless the env asks |
| `0003-signal-host-route.patch` | route host-context faults away from guest SEH; add `thread_peek_emu` |
| `0004-known-abi-dynacall.patch` | `DynaCall_abi` known-ABI entry |
| `0005-signal-jmpbuf-policy.patch` | per-frame signal-mask policy |
| `0006-fini-restore-host-actions.patch` | fini restores pre-box64 host signal actions |
| `0007-bridge-dedup-key.patch` | bridge dedup key + fully initialised slots |
| `0008-serialize-mapping-null-guard.patch` | `SerializeAllMapping()` tolerates an empty mapping table |
| `0009-embedded-endbox64-empty-context.patch` | `endBox64()` tolerates an embedded context with no main ELF |
| `0010-native-module-seam.patch` | engine side of `box64_register_native_module()` in `initWrappedLib()` |
| `0011-embed-libbox64-static-target.patch` | CMake `libbox64` STATIC target, `BOX64_MAIN_SRC` split, generated include lists, `-rdynamic` |
| `0012-plugin-library-list-hook.patch` | `library_list.h` includes the generated plugin table |
| `0013-x87-tagword-fxsave-fidelity.patch` | logical/physical x87 tag word, FXSAVE/FXRSTOR register image, `BOX64_MXCSR_MASK` |
| `0014-la64-fpu-flags-and-16b-atomics.patch` | LA64 dynarec FSQRT C0 and unaligned 16-bit LOCK atomics |
| `0015-la64-next-linker-frame-sentinel.patch` | mark the live `LinkNext` host-stack frame |
| `0016-printer-plugin-signatures.patch` | `x64printer` cases for the seven plugin seam signatures |
| `0017-interpreter-fpu-exception-flags.patch` | interpreter FSQRT sets the x87 C0 flag |
| `0018-compile-portability-includes.patch` | `signals.h` / `dynablock.c` / `rbtree.c` include fixes |

Patches `0001..0010` are the embedded-engine/toolkit changes; `0011..0018`
reconstruct the behaviour of the pre-submodule vendored tree that the previous
`third_party/box64` copy carried as unpatched local edits (`make` could not
rebuild that tree from a pristine checkout). Each header states its rationale.

## Deliberately not in the queue

The queue reproduces every **compiled, behaviour-affecting** difference between
`upstream v0.4.2 + previous patches` and the old vendored tree. The following old
local differences are intentionally absent; they are unreachable in this build
and/or regenerated, not silently dropped:

- **`WINE_BOX64CPU_*` toggles** in `src/tools/env.c`, `src/libtools/sigtools.c`,
  `src/dynarec/dynarec_native_pass.c`, `src/dynarec/la64/dynarec_la64_00.c`,
  `src/dynarec/la64/dynarec_la64_helper.h`,
  `src/dynarec/la64/dynarec_la64_consts.{c,h}` and `src/include/mysignal.h`.
  The macros are defined nowhere in this repository, and the build's
  `compile_commands.json` carries only `-DDYNAREC -DLA64 -DNDEBUG -DSTEP
  -DTEST_INTERPRETER -DBAD_PKILL` (no `_WIN32`, no `WINE_BOX64CPU_*`), so the
  upstream branch is what compiles. The `_WIN32` guards they add are equally
  inactive on the LA64 Linux host.
- **Wine-hosted sources** `src/os/os_wine.c` and `src/os/emit_signal_wine.c`.
  CMake's `OS_LINUX_SRC` selects `os_linux.c` / `emit_signals_linux.c`; the wine
  variants are not compiled here.
- **`src/dynarec/la64/wine_step/`** (64 never-referenced experiment files) and
  **`Makefile.in`** (the autotools file is not in upstream v0.4.2 and no build
  step reads it).
- **`src/emu/x64int3.c`** — the diagnostic block in the old tree is inside
  `#if 0`.
- **Generated wrapper heads** — `src/wrapped/generated/{functions_list.txt,
  wrapper.c,wrapper.h,wrapped*defs.h,wrapped*types.h,wrapped*undefs.h}` and
  `wrapper_min.c` are produced by box64's own `rebuild_wrappers.py` from the
  `*_private.h` files the build passes it (including the grafted
  `wrappedbox64dnative_private.h` and `plugins/libgccs`). The seven new
  signatures patch 0016 prints all come from those private headers, so the heads
  are regenerated rather than patched. The old tree's `wrappedlibnode*` heads
  came from a `libnode` plugin that is not part of this repository.
- **Build residue** — `*.orig` patch pre-images, `build/`, `build-pic/`,
  `rust/target/`, `src/git_head.h` and the patch stamp are ignored by
  `.gitmodules` (`ignore = untracked`) and never tracked.

## Relationship to the old `third_party/box64` copy

`docs/vendor-provenance.md` records how the pre-submodule copy was audited
against this queue: pristine v0.4.2 plus `0001..0010` differed from the old tree
in 27 files, 12 tree-only files and 21 upstream files; `0011..0018` close the 12
behaviour-affecting files, and the list above is the remainder.
