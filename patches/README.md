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

Patches `0001..0010` are the embedded-engine/toolkit changes; `0011..0018` are
build and behaviour fixes against pristine upstream. Each header states its
rationale.

Build residue — `*.orig` patch pre-images, `build/`, `build-pic/`,
`rust/target/`, `src/git_head.h` and the patch stamp — is ignored by
`.gitmodules` (`ignore = untracked`) and never tracked. The generated wrapper
heads under `src/wrapped/generated/` are produced by box64's own
`rebuild_wrappers.py` from the grafted `*_private.h` files, so they are
regenerated rather than patched.
