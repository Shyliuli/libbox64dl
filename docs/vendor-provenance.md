# Vendored box64 and source provenance (E1)

This file records where the E1 tree came from. It deliberately does **not** invent
an upstream commit for the vendored engine.

## Source snapshot

- Snapshot id: `20260925T124449Z`
- Archive: `.rebuild/snapshots/20260925T124449Z/archives/libbox64dl-src.tar.gz`
- sha256: `6e051c64d52d48acb4394bbb786de70169bd954cac445d3ac1520ac69cf69b43`
- Entries: 2427 (tracked + non-ignored untracked files of the old worktree;
  `build*/`, caches and absolute graft symlinks excluded)
- Old repository HEAD at snapshot time:
  `libbox64dl 6a8604d2e030261b7854def96d3ce09b4873cae5` (branch `test`)

That HEAD is a **development snapshot**, not a release baseline, and it is not a
parent of this tree: the V2 delivery model publishes each repository as a fresh
root commit sequence, so the old history is reference only.

## Vendored engine

- Path: `third_party/box64/`
- Origin: the tracked vendor tree inside the old `libbox64dl` repository
  (2363 tracked files at snapshot time).
- **Upstream commit/version is not independently verifiable from this tree**, so
  no upstream SHA is recorded here. The vendor tree is redistributed under its
  own license and copyright: see `third_party/box64/LICENSE`.
- Local vendor changes are the numbered patches in `patches/` only:

  | patch | subject |
  | --- | --- |
  | `0001-export-openFTrace.patch` | export `openFTrace` for the Rust bootstrap |
  | `0002-silent-log-default-unless-env.patch` | default-silent logging unless the env asks |
  | `0003-signal-host-route.patch` | route host-context faults away from guest SEH; add `thread_peek_emu` |
  | `0004-known-abi-dynacall.patch` | `DynaCall_abi` known-ABI entry |
  | `0005-signal-jmpbuf-policy.patch` | per-frame signal-mask policy |
  | `0006-fini-restore-host-actions.patch` | fini restores pre-box64 host signal actions |
  | `0007-bridge-dedup-key.patch` | bridge dedup key + fully initialised slots |
  | `0008-serialize-mapping-null-guard.patch` | `SerializeAllMapping()` tolerates an empty mapping table (embedded init→fini without a guest dlopen) |
  | `0009-embedded-endbox64-empty-context.patch` | `endBox64()` tolerates an embedded context that never loaded a main ELF |

  Each patch header states its own rationale and scope; the patch set is applied
  by `make patch` and reversed by `make unpatch`.
- Beyond the patches, the only edits inside the vendored tree are the two
  include hooks already tracked there: `src/library_list.h` (includes the
  generated `library_list_extra.h`) and `CMakeLists.txt` (includes the generated
  `box64dl_core.cmake` / `box64dl_plugins.cmake`). Everything else under
  `third_party/box64` is re-syncable with upstream.

## In-repo plugin

E1's only wrapped-library plugin is `plugins/libgccs` (host `libgcc_s.so.1`
unwinding / quad-math symbols), which lives in this repository. No external or
Node-side plugin is part of the E1 build; see `tools/check_no_node.sh` and
`README.md` ("wrapped library 插件").
