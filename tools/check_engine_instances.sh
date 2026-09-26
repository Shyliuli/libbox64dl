#!/bin/sh
# check_engine_instances.sh — E1 gate: the built DSO contains exactly one box64
# engine instance (one `my_context`) and one definition of each public entry.
set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
dso="$root/build/lib/libbox64dl.so"
[ -f "$dso" ] || { echo "check-engine-instances: no $dso (run 'make shared')" >&2; exit 1; }

count_sym() {
  # shellcheck disable=SC2016
  nm "$dso" | awk -v s="$1" '$NF==s && $2 ~ /^[BbDdGgRrTt]$/ {n++} END {print n+0}'
}

fail=0
n=$(count_sym my_context)
[ "$n" -eq 1 ] || { echo "check-engine-instances: FAIL: my_context defined $n times" >&2; fail=1; }
n=$(count_sym box64_call_simple)
[ "$n" -eq 1 ] || { echo "check-engine-instances: FAIL: box64_call_simple defined $n times" >&2; fail=1; }
n=$(count_sym box64_lib_init)
[ "$n" -eq 1 ] || { echo "check-engine-instances: FAIL: box64_lib_init defined $n times" >&2; fail=1; }

[ "$fail" -eq 0 ] || exit 1
echo "check-engine-instances: PASS (single box64 context, single public entry set)"
