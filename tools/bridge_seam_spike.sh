#!/bin/sh
# bridge_seam_spike.sh — cross-repository link spike for the native-module seam.
#
# Builds tests/bridge_seam_spike_main.c together with the sibling provider
# tree's REAL seam sources and the REAL engine archive, then runs it. This is
# the check that the engine's exported box64_register_native_module() resolves
# the provider's weak declaration and accepts the provider's real descriptor.
#
# The provider tree defaults to ../../../uninode/native relative to this
# repository (the candidate checkout lives under .rebuild/work/libbox64dl).
# Override with BRIDGE_PROVIDER_DIR. A missing provider tree is a hard FAIL:
# this target exists to run that link, so "nothing to check" must never pass.
set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
# Default provider tree: the live sibling checkout (../uninode/native), with the
# .rebuild/work/libbox64dl candidate layout as a fallback.
provider=${BRIDGE_PROVIDER_DIR:-$root/../uninode/native}
[ -d "$provider" ] || provider=$root/../../../uninode/native
provider_wrapped=$(cd "$provider/../wrapped" 2>/dev/null && pwd) || provider_wrapped=""
cc=${CC:-cc}

for f in uninode_native_module.h uninode_native_module.c; do
  [ -f "$provider/$f" ] || {
    echo "bridge-seam-spike: FAIL: missing $provider/$f (set BRIDGE_PROVIDER_DIR)" >&2
    exit 1
  }
done
[ -n "$provider_wrapped" ] && [ -f "$provider_wrapped/wrappedlibnode.c" ] || {
  echo "bridge-seam-spike: FAIL: missing $provider/../wrapped/wrappedlibnode.c (set BRIDGE_PROVIDER_DIR)" >&2
  exit 1
}

engine="$root/vendor/box64/build/libbox64.a"
dynarec="$root/vendor/box64/build/libdynarec.a"
rust="$root/rust/target/static/debug/liblibbox64.a"
for f in "$engine" "$dynarec" "$rust"; do
  [ -f "$f" ] || { echo "bridge-seam-spike: FAIL: missing $f (run 'make static' and 'make rust-static')" >&2; exit 1; }
done

out="$root/build/bridge_seam_spike"
mkdir -p "$(dirname "$out")"

set -- \
  -Og -g -std=c11 -Wall -Wextra -D_GNU_SOURCE \
  -I"$root/include" -I"$provider" -I"$provider_wrapped" \
  -I"$root/vendor/box64/src/include" -I"$root/vendor/box64/src" \
  -o "$out" "$root/tests/bridge_seam_spike_main.c" \
  "$root/tests/bridge_seam_spike_engine.c" \
  "$provider/uninode_native_module.c" "$provider_wrapped/wrappedlibnode.c" \
  -Wl,--whole-archive "$engine" "$dynarec" -Wl,--no-whole-archive "$rust" \
  -lpthread -lm -ldl -lrt -lresolv -lutil -rdynamic
echo "bridge-seam-spike: $cc $*"
$cc "$@"

"$out"
