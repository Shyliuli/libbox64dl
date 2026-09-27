# Vendored box64 and source provenance

The engine is the **upstream** box64 repository, checked out as the git submodule
`vendor/box64`, plus the ordered patch queue in `patches/`. The pre-submodule
vendored copy under `third_party/box64` has been retired; everything it carried
is either in the patch queue or listed below as provably unreachable in this
build.

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
describes the queue and the changes it deliberately does **not** carry; the
version lock is `patches/box64.lock`.

Verify the submodule commit, the patch hashes, a pristine apply and the live
worktree in one shot:

```sh
tools/verify_box64_patch.sh --check
```

## How the queue was reconstructed

The queue was built by comparing three disposable trees (nothing was edited in
this repository's worktree to derive it):

1. pristine `git archive` of box64 `v0.4.2`;
2. that tree plus `patches/0001..0010` (all ten applied cleanly);
3. the retired tracked `third_party/box64` at commit
   `e4cfd28` (`git archive HEAD third_party/box64`).

Tree 2 vs tree 3: **27 files differ, 12 files exist only in tree 3, and 21
upstream files/directories were absent** from tree 3 (upstream docs, packaging,
`tests/`, `wine/`, `x64lib/`, `system/`, … — the old vendor copy was pruned).

- The 12 tree-only/behaviour-affecting files are exactly what `0011..0018`
  restore; after applying the full queue, every one of them is byte-identical to
  tree 3.
- The missing upstream files are simply back: the submodule is a complete
  upstream checkout, and no build step depends on them.
- The remaining 27-file differences are the unreachable changes enumerated in
  `patches/README.md` (never-defined `WINE_BOX64CPU_*` macros, `_WIN32`-only
  paths, the two Wine-hosted `src/os` files CMake does not compile,
  `wine_step/` + `Makefile.in`, the `#if 0` block in `x64int3.c`, and the
  generated wrapper heads that `rebuild_wrappers.py` regenerates from the
  grafted private headers).

The `WINE_BOX64CPU_*` claim was checked against the real build, not only by
reading: the retired tree's `compile_commands.json` defines only `DYNAREC`,
`LA64`, `NDEBUG`, `STEP`, `TEST_INTERPRETER` and `BAD_PKILL`, and no
`_WIN32`/`WINE_BOX64CPU_*` macro appears anywhere in this repository.

## Pre-submodule snapshot (historical)

The retired `third_party/box64` copy came from the E1 snapshot recorded below;
it is kept here because it documents the tree the patch queue was audited
against.

- Snapshot id: `20260925T124449Z`
- Archive: `.rebuild/snapshots/20260925T124449Z/archives/libbox64dl-src.tar.gz`
- sha256: `6e051c64d52d48acb4394bbb786de70169bd954cac445d3ac1520ac69cf69b43`
- Old repository HEAD at snapshot time:
  `libbox64dl 6a8604d2e030261b7854def96d3ce09b4873cae5` (branch `test`)
- Vendored copy at the migration commit: `third_party/box64`,
  2363 files, no upstream SHA of its own — which is precisely why it could not
  be rebuilt from upstream and was replaced by the submodule + queue above.

## Build caches

`vendor/box64/build`, `vendor/box64/build-pic`, `rust/target` and the
verifier's `build/verify-box64` scratch tree are build output. They live inside
the submodule or under this repository's ignored `build/`, are never committed,
and are not shared between repositories; `make clean` removes the two box64
trees and `make distclean` also reverses the patch queue.
