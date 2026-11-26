#!/usr/bin/env bash
set -euo pipefail

ARCH_FLAG=""
case "$(uname -m)" in
    loongarch64) ARCH_FLAG="-DLARCH64" ;;
    aarch64)     ARCH_FLAG="-DARM64" ;;
    riscv64)     ARCH_FLAG="-DRV64" ;;
    ppc64le)     ARCH_FLAG="-DPPC64LE" ;;
    x86_64)      ARCH_FLAG="" ;;
    *) echo "unsupported arch: $(uname -m)" >&2; exit 1 ;;
esac

bindgen src/box64dl_bindgen.h \
    --rust-target 1.82 \
    --opaque-type '.*' \
    --no-layout-tests \
    -o src/box64_bindings.rs \
    -- -Ithird_party/box64/src/include $ARCH_FLAG
