#!/bin/sh
# libbox64dl — vendor/box64 submodule commit + patch-queue verifier.
#
# WHAT THIS PINS
#   The engine is the *upstream* box64 repository
#       https://github.com/ptitSeb/box64.git
#   checked out as the git submodule vendor/box64 at tag v0.4.2 (commit below).
#   Nothing in the submodule is committed locally: `make patch` applies the
#   ordered queue in patches/ to that pristine checkout and `make graft` links
#   this repository's sources into it.
#
# WHAT IT CHECKS (no checksums are recorded anywhere)
#   - vendor/box64 HEAD is the pinned upstream commit
#   - every patch applies, in filename order, to a pristine export of that
#     commit (git archive into build/verify-box64)
#   - every file a patch touches is byte-identical to the current vendor/box64
#     working tree, so an edit made without regenerating the patch fails here
#
#   The last two are direct file comparisons. A recorded hash would only add a
#   second thing to keep in step, and it is the export-and-compare that actually
#   proves the worktree is reproduced by the queue.
#
# USAGE
#   tools/verify_box64_patch.sh [--vendor-tree DIR]
#
# EXIT
#   0 pass; 1 verification failed; 2 usage/setup error.
set -u

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
VENDOR_TREE=$ROOT/vendor/box64
PATCH_DIR=$ROOT/patches
UPSTREAM_REMOTE=https://github.com/ptitSeb/box64.git
UPSTREAM_TAG=v0.4.2
UPSTREAM_COMMIT=7eeb5016493dab4e143d53da50dd47bfb44a9509

while [ $# -gt 0 ]; do
  case $1 in
    --check) ;;   # the default and only mode; accepted for compatibility
    --vendor-tree) shift; [ $# -gt 0 ] || { echo "verify_box64_patch: --vendor-tree needs a value" >&2; exit 2; }; VENDOR_TREE=$1 ;;
    -h|--help) sed -n '2,/^set -u$/p' "$0"; exit 0 ;;
    *) echo "verify_box64_patch: unknown argument: $1" >&2; exit 2 ;;
  esac
  shift
done

die() { echo "verify_box64_patch: $*" >&2; exit "${2:-1}"; }
err() { echo "verify_box64_patch: $*" >&2; exit 2; }

[ -d "$PATCH_DIR" ] || err "no patch directory: $PATCH_DIR"
[ -f "$VENDOR_TREE/CMakeLists.txt" ] || err "$VENDOR_TREE is not a box64 checkout; run 'make vendor'"

patches=$(ls "$PATCH_DIR"/*.patch 2>/dev/null | sort)
[ -n "$patches" ] || err "no patches under $PATCH_DIR"

# `git status`/`rev-parse` must run in the submodule itself.
sub_commit=$(git -C "$VENDOR_TREE" rev-parse HEAD 2>/dev/null) \
  || err "$VENDOR_TREE is not a git checkout (submodule not initialised?)"

[ "$sub_commit" = "$UPSTREAM_COMMIT" ] \
  || die "vendor/box64 HEAD=$sub_commit, expected upstream $UPSTREAM_TAG ($UPSTREAM_COMMIT)"

patch_count=$(printf '%s\n' "$patches" | wc -l | tr -d ' ')

# Apply the queue to a pristine export of the pinned commit and compare every
# patched file with the live worktree.
tmpbase=$ROOT/build/verify-box64
rm -rf "$tmpbase"
mkdir -p "$tmpbase/pristine" || die "cannot create $tmpbase"
trap 'rm -rf "$tmpbase"' 0 HUP INT TERM

git -C "$VENDOR_TREE" archive HEAD | tar -x -C "$tmpbase/pristine" \
  || die "cannot export pristine $sub_commit"

for p in $patches; do
  name=$(basename "$p")
  ( cd "$tmpbase/pristine" && patch -p1 -N --no-backup-if-mismatch -i "$p" >"$tmpbase/$name.log" 2>&1 ) \
    || { echo "verify_box64_patch: $name does not apply to pristine $sub_commit:" >&2; sed -n '1,40p' "$tmpbase/$name.log" >&2; exit 1; }
done

failed=0
for p in $patches; do
  for f in $(sed -n 's|^+++ b/||p' "$p" | sed 's/[[:space:]]*$//' | sort -u); do
    [ "$f" = /dev/null ] && continue
    pristine="$tmpbase/pristine/$f"
    live="$VENDOR_TREE/$f"
    [ -e "$pristine" ] || die "$(basename "$p"): patched file $f does not exist"
    if [ ! -e "$live" ]; then
      echo "verify_box64_patch: FAIL $f missing from vendor/box64" >&2
      failed=1
    elif ! cmp -s "$pristine" "$live"; then
      echo "verify_box64_patch: FAIL $f differs from the patch queue result (edit vendor/box64 or regenerate the patch)" >&2
      failed=1
    fi
  done
done
[ "$failed" -eq 0 ] || exit 1

echo "verify_box64_patch: ok — $patch_count patches over upstream box64 $UPSTREAM_TAG ($sub_commit)"
exit 0
