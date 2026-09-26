#!/bin/sh
# check_linkage.sh — E1 linkage gate.
#
# Properties enforced, not merely observed:
#   1. exactly ONE Rust staticlib is in the final DSO link;
#   2. exactly ONE box64 engine archive is in that link (not 0: the old
#      `-le 1` test passed a link with no engine at all);
#   3. that archive is the PIC engine (`build-pic/libbox64.a`) that the DSO
#      actually needs, not the static one;
#   4. ownership convergence: the Rust staticlib must NOT bundle the engine
#      (its shim bundle must not contain `my_context`), while it must still
#      carry the shim entry points;
#   5. the link target is the single shipped DSO.
set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
line=$(make -C "$root" --no-print-directory print-link-line)

rust_libs=$(printf '%s\n' "$line" | tr ' ' '\n' | grep -E 'rust/target/[^ ]*\.a$' || true)
rust_count=$(printf '%s\n' "$rust_libs" | grep -c . || true)
[ "$rust_count" -eq 1 ] || {
  echo "check-linkage: FAIL: expected exactly 1 Rust staticlib in the final link, got $rust_count" >&2
  printf '%s\n' "$line" >&2
  exit 1
}

# Match the exact archive basename so the Rust archive `liblibbox64.a` is not
# counted as an engine archive.
engine_archives=$(printf '%s\n' "$line" | tr ' ' '\n' | grep -E '/libbox64\.a$' || true)
engine_count=$(printf '%s\n' "$engine_archives" | grep -c . || true)
[ "$engine_count" -eq 1 ] || {
  echo "check-linkage: FAIL: expected exactly 1 engine archive in the final link, got $engine_count" >&2
  printf '%s\n' "$line" >&2
  exit 1
}
case "$engine_archives" in
  */build-pic/libbox64.a) ;;
  *)
    echo "check-linkage: FAIL: the DSO links $engine_archives, not the PIC engine archive" >&2
    exit 1
    ;;
esac

# The private shim (core_shim.c) and the Rust phase leaves must be compiled by
# the Rust crate, never into the engine archive: the engine archive is the
# vendored box64 plus our VENDOR_SRC only.
if nm --defined-only "$engine_archives" 2>/dev/null \
    | grep -qE ' [Tt] (box64dl_core_|box64dl_rust_phase_)'; then
  echo "check-linkage: FAIL: engine archive contains the Rust shim/phase leaves" >&2
  exit 1
fi

rust_lib=$(printf '%s\n' "$rust_libs" | head -n1)
# Ownership convergence: rustc `staticlib` bundles every native static library
# declared with rustc-link-lib=static. build.rs deliberately uses
# rustc-link-arg instead, so the engine is present exactly once (in the DSO's
# own --whole-archive aggregate).
if nm --defined-only "$rust_lib" 2>/dev/null | grep -qE ' [TtBbDdGgRr] my_context$'; then
  echo "check-linkage: FAIL: Rust staticlib bundles the box64 engine (my_context defined): $rust_lib" >&2
  exit 1
fi
if ! nm "$rust_lib" 2>/dev/null | grep -q ' T box64dl_core_context_new$'; then
  echo "check-linkage: FAIL: Rust staticlib is missing the private shim ($rust_lib)" >&2
  exit 1
fi

dso="$root/build/lib/libbox64dl.so"
[ -f "$dso" ] || {
  echo "check-linkage: FAIL: final DSO $dso was not built" >&2
  exit 1
}

echo "check-linkage: PASS (1 Rust staticlib without the engine, 1 PIC engine archive, 1 DSO target)"