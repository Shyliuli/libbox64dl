#!/bin/sh
# check_no_node.sh — fail if the CURRENT libbox64dl build depends on Node.
#
# Scope: the active graft/provider and the built artifact, never the history of
# the vendored tree. The vendored box64 snapshot carries tracked upstream
# generated headers whose names contain `libnode`
# (src/wrapped/generated/wrappedlibnode{defs,undefs,types}.h), and an earlier
# build may have left `build/**/wrappedlibnode.c.o` plus dangling graft
# symlinks behind. None of those is part of the current graft, so none of them
# is a failure here — and deleting them is explicitly NOT this script's job,
# since it must not touch tracked upstream files.
#
# Checked, fail-closed:
#   1. active graft/provider: no plugins/libnode entry, no *resolvable*
#      symlink reaching uninode or another libnode provider;
#   2. the generated build inputs exist AND name no Node/uv source;
#   3. no Node/sibling-repository reverse dependency in the build entry points
#      (Makefile, rust/Cargo.toml, rust/build.rs);
#   4. no own C/asm source or Rust build script includes a Node/uv header;
#   5. the final DSO has no Node/uv DT_NEEDED and no Node-side dynamic symbol.
# A missing generated build input or a missing DSO is a FAIL (run the build),
# never a silent skip that turns "nothing to check" into a PASS.
set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
box="$root/third_party/box64"
dso="$root/build/lib/libbox64dl.so"
fail=0

note() { echo "check-no-node: FAIL: $*" >&2; fail=1; }

# 1a. The active plugin set is exactly the in-repo libgccs plugin. Any other
#     provider fails; a libnode entry fails even when dangling, because that is
#     the path graft resolves next.
for d in "$root"/plugins/*; do
  [ -e "$d" ] || [ -L "$d" ] || continue
  name=$(basename "$d")
  [ "$name" = "libgccs" ] || note "unexpected plugin provider: plugins/$name"
done

# 1b. No ACTIVE symlink reaches a Node provider. A dangling symlink is an
#     inactive graft leftover, not a current dependency, so it is not a FAIL.
while IFS= read -r link; do
  target=$(readlink -f "$link" 2>/dev/null) || continue
  [ -e "$target" ] || continue
  case "$target" in
    */uninode/*|*/uninode|*libnode*)
      note "active symlink into a Node provider: ${link#"$root"/} -> $target" ;;
  esac
done <<EOF
$(find "$root/plugins" "$box/src" -type l -print 2>/dev/null)
EOF

# 1c. Informational only: name-matched leftovers in the vendored tree
#     (upstream generated headers, old build cache, dangling graft symlinks)
#     are stale state, not the active graft.
stale=$(find "$box" -name '*libnode*' 2>/dev/null | wc -l)
[ "$stale" -eq 0 ] || echo "check-no-node: note: $stale stale *libnode* artifact(s) under third_party/box64 are not the active graft"

# 2. The generated build inputs are REQUIRED. Absent means the build was not
#    run for this check; skipping them would fake a PASS. Present ones are the
#    authoritative list of the grafted sources, so a Node/uv name in them is a
#    real current build input.
for f in "$box/box64dl_plugins.cmake" "$box/box64dl_core.cmake" \
         "$box/src/library_list_extra.h"; do
  if [ ! -f "$f" ]; then
    note "generated build input missing: $f (run 'make graft')"
    continue
  fi
  if grep -qi 'libnode\|uninode\|napi_\|libuv\|uv\.h' "$f"; then
    note "generated build input mentions Node/uv: $f"
  fi
done

# 3. Build entry points never reach into a sibling repository and never need
#    Node headers. Checked across the Makefile, the Rust crate manifest and the
#    crate's build script (the actual dependency declaration). The Makefile scan
#    excludes comments and check_exports.py's negative-injection list, which
#    names Node symbols on purpose.
if grep -nE 'uninode|winedl|NODE_INC|node_modules|node-|libuv|-lnode' \
    "$root/Makefile" | grep -v -- '--inject-symbol' >/dev/null 2>&1; then
  note "build entry point references a Node/sibling-repository dependency: $root/Makefile"
fi
for f in "$root/rust/Cargo.toml" "$root/rust/build.rs"; do
  if grep -nE 'uninode|winedl|node-sys|napi|libuv|"uv"' "$f" >/dev/null 2>&1; then
    note "build entry point references a Node/sibling-repository dependency: $f"
  fi
done

# 4. Our own translation units (including plugins) never include a Node header.
if grep -rnE '#[[:space:]]*include[[:space:]]*[<"](node|napi|uv)[^>"]*[>"]' \
    "$root/src" "$root/include" "$root/cli" "$root/plugins" \
    "$root/rust/build.rs" >/dev/null 2>&1; then
  note "a source file includes a Node/uv header"
fi

# 5. The linked DSO: no NEEDED on a Node/uv library and no Node-side dynamic
#    symbol (defined or undefined).
if [ ! -f "$dso" ]; then
  note "final DSO missing: $dso (run 'make shared')"
else
  if readelf -d "$dso" 2>/dev/null | grep -iE 'NEEDED.*(libnode|libuv|node\.so)' >/dev/null; then
    note "DSO has a Node/uv DT_NEEDED entry"
  fi
  if nm -D "$dso" 2>/dev/null | grep -qiE ' (napi_|uv_|node_|uninode|winedl_)'; then
    note "DSO carries a Node-side dynamic symbol"
  fi
fi

if [ "$fail" -ne 0 ]; then
  exit 1
fi
echo "check-no-node: PASS (active graft/provider, generated build inputs and linked DSO free of any Node dependency)"
