#!/bin/sh
# libbox64dl — vendor/box64 submodule commit + patch-queue verifier.
#
# WHAT THIS LOCKS
#   The engine is the *upstream* box64 repository
#       https://github.com/ptitSeb/box64.git
#   checked out as the git submodule vendor/box64 at tag v0.4.2
#   (commit 7eeb5016493dab4e143d53da50dd47bfb44a9509). Nothing in the submodule
#   is committed locally: `make patch` applies the ordered queue in patches/ to
#   that pristine checkout and `make graft` links this repository's sources into
#   it. The pair (submodule commit, patch files) is the whole engine definition,
#   so both halves are recorded here:
#
#       patches/box64.lock        the submodule commit + patch sha256s
#       patches/*.patch           the queue, in apply order
#
#   A pristine checkout of box64_commit + these patches reproduces the engine
#   worktree this repository builds. None of the patches are upstream; do not
#   present them as an upstream box64 change.
#
# MODES
#   --check (default)  verify without writing under vendor/box64:
#                        - vendor/box64 HEAD == box64_commit from the lock
#                        - every patch file exists and matches its recorded sha256
#                        - the queue is complete and in the recorded order
#                        - every patch applies to a pristine export of the
#                          submodule commit (git archive, in a temp dir under
#                          build/), in order, with GNU patch
#                        - every file a patch touches is byte-identical to the
#                          current vendor/box64 working tree, so an edit made
#                          without regenerating the patch fails here
#   --write            regenerate box64.lock from the current patch files and
#                      the current vendor/box64 HEAD (read-only on the submodule
#                      itself).
#
# USAGE
#   tools/verify_box64_patch.sh [--check|--write] [--vendor-tree DIR]
#
# EXIT
#   0 pass; 1 verification failed; 2 usage/setup error.
set -u

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
MODE=check
VENDOR_TREE=$ROOT/vendor/box64
PATCH_DIR=$ROOT/patches
LOCK=$PATCH_DIR/box64.lock
UPSTREAM_REMOTE=https://github.com/ptitSeb/box64.git
UPSTREAM_TAG=v0.4.2

while [ $# -gt 0 ]; do
  case $1 in
    --check) MODE=check ;;
    --write) MODE=write ;;
    --vendor-tree) shift; [ $# -gt 0 ] || { echo "verify_box64_patch: --vendor-tree needs a value" >&2; exit 2; }; VENDOR_TREE=$1 ;;
    -h|--help) sed -n '2,48p' "$0"; exit 0 ;;
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

patch_count=$(printf '%s\n' "$patches" | wc -l | tr -d ' ')

# ---------------------------------------------------------------- lock write
if [ "$MODE" = write ]; then
  tmp=$LOCK.tmp
  {
    echo "# vendor/box64 version lock — UPSTREAM SUBMODULE + LOCAL PATCH QUEUE."
    echo "# Regenerate with: tools/verify_box64_patch.sh --write"
    echo "# Verify with:     tools/verify_box64_patch.sh --check"
    echo "kind = upstream-submodule-patches"
    echo "generated_utc = $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "upstream_remote = $UPSTREAM_REMOTE"
    echo "upstream_tag = $UPSTREAM_TAG"
    echo "box64_commit = $sub_commit"
    echo "patch_count = $patch_count"
    i=0
    for p in $patches; do
      i=$((i + 1))
      echo "patch.$i = $(basename "$p") $(sha256sum "$p" | cut -d' ' -f1)"
    done
  } > "$tmp" || die "cannot write $tmp"
  mv "$tmp" "$LOCK" || die "cannot install $LOCK"
  echo "verify_box64_patch: wrote $LOCK ($patch_count patches, box64 $sub_commit)"
  exit 0
fi

# ---------------------------------------------------------------- lock check
[ -f "$LOCK" ] || err "missing lock $LOCK; run tools/verify_box64_patch.sh --write"
lock_get() { sed -n "s/^$1 = //p" "$LOCK" | head -1; }

locked_commit=$(lock_get box64_commit)
[ -n "$locked_commit" ] || die "lock has no box64_commit"
[ "$locked_commit" = "$sub_commit" ] \
  || die "vendor/box64 HEAD=$sub_commit, lock says $locked_commit"

locked_remote=$(lock_get upstream_remote)
[ "$locked_remote" = "$UPSTREAM_REMOTE" ] \
  || die "lock upstream_remote=$locked_remote, expected $UPSTREAM_REMOTE"

locked_count=$(lock_get patch_count)
[ "$locked_count" = "$patch_count" ] \
  || die "patch_count: lock says $locked_count, found $patch_count files"

# Patch names/hashes, in order: patch.N lines must name the sorted files.
i=0
for p in $patches; do
  i=$((i + 1))
  line=$(sed -n "s/^patch\.$i = //p" "$LOCK" | head -1)
  [ -n "$line" ] || die "lock has no patch.$i"
  want_name=${line%% *}
  want_hash=${line#* }
  [ "$want_name" = "$(basename "$p")" ] \
    || die "patch.$i: lock names $want_name, found $(basename "$p")"
  have_hash=$(sha256sum "$p" | cut -d' ' -f1)
  [ "$have_hash" = "$want_hash" ] \
    || die "patch $want_name sha256 drifted (lock $want_hash, file $have_hash); run --write after a deliberate edit"
done

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
