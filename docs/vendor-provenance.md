# Vendored box64 and source provenance

The engine is the **upstream** box64 repository, checked out as the git submodule
`vendor/box64`, plus the ordered patch queue in `patches/`.

## Engine source (upstream submodule)

- Path: `vendor/box64/`
- Origin: <https://github.com/ptitSeb/box64.git>
- Tag: `v0.4.2`
- Commit: `7eeb5016493dab4e143d53da50dd47bfb44a9509`
- Pinned by the gitlink in `.gitmodules`; `make check-vendor` fails if the
  submodule HEAD is not that commit, and `make vendor` materialises it
  (`git submodule update --init --recursive vendor/box64`).

On a machine without network access, `make vendor` can be pointed at a local
mirror of the public repository; git's `file://` transport has to be enabled
explicitly for a submodule clone:

```sh
make vendor BOX64_GIT_OPTS='-c protocol.file.allow=always \
  -c url."/path/to/box64".insteadOf=https://github.com/ptitSeb/box64.git'
```

This was verified against a local mirror: the fresh `git submodule update --init`
checks out `7eeb5016493dab4e143d53da50dd47bfb44a9509` (`v0.4.2`) and the resulting
tree is identical to `vendor/box64` here.
- Licence and copyright are upstream's: `vendor/box64/LICENSE`.

Nothing under `vendor/box64` is committed locally. `make patch` applies
`patches/0001..0018` into the submodule worktree, `make graft` links this
repository's sources into it, and `make` builds under
`vendor/box64/build{,-pic}`. The submodule is therefore *expected* to be dirty;
`.gitmodules` sets `ignore = untracked` so its build trees, graft symlinks,
generated heads and patch stamp are not superproject content, while the submodule
pointer itself stays visible.

Local engine changes are the numbered patches in `patches/` only:

| patch | subject |
| --- | --- |
| `0001` | export `openFTrace` for the Rust bootstrap |
| `0002` | default-silent logging unless the env asks |
| `0003` | route host-context faults away from guest SEH; add `thread_peek_emu` |
| `0004` | `DynaCall_abi` known-ABI entry |
| `0005` | per-frame signal-mask policy |
| `0006` | fini restores pre-box64 host signal actions |
| `0007` | bridge dedup key + fully initialised slots |
| `0008` | `SerializeAllMapping()` tolerates an empty mapping table |
| `0009` | `endBox64()` tolerates an embedded context that never loaded an ELF |
| `0010` | engine side of `box64_register_native_module()` in `initWrappedLib()` |
| `0011` | CMake `libbox64` STATIC target, `BOX64_MAIN_SRC`, generated include lists, `-rdynamic` |
| `0012` | `library_list.h` includes the generated plugin table |
| `0013` | x87 tag word + FXSAVE/FXRSTOR register image fidelity |
| `0014` | LA64 dynarec FSQRT C0 and unaligned 16-bit LOCK atomics |
| `0015` | mark the live `LinkNext` host-stack frame |
| `0016` | `x64printer` cases for the plugin seam signatures |
| `0017` | interpreter FSQRT sets the x87 C0 flag |
| `0018` | `signals.h` / `dynablock.c` / `rbtree.c` include portability |

Each patch header states its own rationale and scope. `patches/README.md`
describes the queue and the changes it deliberately does **not** carry.

Verify the submodule commit, a pristine apply and the live worktree in one shot:

```sh
tools/verify_box64_patch.sh --check
```

## Build caches

`vendor/box64/build`, `vendor/box64/build-pic`, `rust/target` and the
verifier's `build/verify-box64` scratch tree are build output. They live inside
the submodule or under this repository's ignored `build/`, are never committed,
and are not shared between repositories; `make clean` removes the two box64
trees and `make distclean` also reverses the patch queue.
