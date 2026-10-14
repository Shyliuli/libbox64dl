# libbox64dl — box64 as an embeddable library for loading x86_64 shared objects.
#
# The engine is the upstream box64 Submodule at vendor/box64, pinned to v0.4.2
# (commit 7eeb5016493dab4e143d53da50dd47bfb44a9509) by the gitlink in
# .gitmodules. `make patch` applies patches/ into that checkout; `make graft`
# links this repository's sources into it. Nothing under vendor/box64 is
# hand-edited in git: the submodule worktree is expected to be dirty.
#
# Two flavours are built from one vendored tree:
#   build/      static, dynarec allowed — for host executables (unilib tests)
#   build-pic/  -fPIC, interpreter only — for .so consumers (embedded engine)

ROOT := $(abspath .)
BOX64 := $(ROOT)/vendor/box64
# The pinned upstream commit. `make check-vendor` compares it with the
# submodule's HEAD so a moved or uninitialised submodule fails loudly instead of
# building a different engine than the patch queue was written for.
BOX64_REF := 7eeb5016493dab4e143d53da50dd47bfb44a9509
# Host/target architecture.
#   aarch64      ARM64 host, upstream ARM64 dynarec (native)
#   loongarch64  LARCH64=ON, the original target
#   x86_64       interpreter-only debug target
# Detected from `uname -m`; override with LIBBOX64DL_ARCH=... when cross-building.
HOST_MACHINE ?= $(shell uname -m)
LIBBOX64DL_ARCH ?= $(if $(filter x86_64 amd64,$(HOST_MACHINE)),x86_64,$(if $(filter aarch64 arm64,$(HOST_MACHINE)),aarch64,loongarch64))
ARCH_SUFFIX := $(if $(filter x86_64 amd64,$(LIBBOX64DL_ARCH)),-x64,)
BUILD_STATIC := $(BOX64)/build$(ARCH_SUFFIX)
BUILD_PIC := $(BOX64)/build-pic$(ARCH_SUFFIX)
NPROC := $(shell nproc)
RUST_TARGET := $(if $(filter x86_64 amd64,$(LIBBOX64DL_ARCH)),x86_64-unknown-linux-gnu,)
RUST_TARGET_ARG := $(if $(RUST_TARGET),--target $(RUST_TARGET),)
RUST_TARGET_SUFFIX := $(if $(RUST_TARGET),-x64,)
DYNAREC_STATIC := $(wildcard $(BUILD_STATIC)/libdynarec.a)
DYNAREC_PIC := $(wildcard $(BUILD_PIC)/libdynarec.a)
NATIVE_RUN_ENV := $(if $(filter ON on 1,$(LIBBOX64_NATIVE_X64)),BOX64_X64_NATIVE=1,)

# Ordered patches against the pristine submodule checkout (see patches/).
PATCHES := $(sort $(wildcard $(ROOT)/patches/*.patch))
PATCH_STAMP := $(BOX64)/.libbox64dl_patches_stamp

# Plugin sources. Each plugin directory supplies wrapped<lib>.c,
# wrapped<lib>_private.h and library_list.inc.
#
# Default (no plugins.conf): every plugins/<name>/ entry. E1 ships exactly one
# in-repo plugin (libgccs); no sibling-repository symlink is part of this tree.
#
# For an explicit, reviewable set, write plugins.conf with one `name = path`
# line per plugin (`#` comments and blank lines ignored; path absolute or
# relative to this repo and free of spaces) and pass it as PLUGIN_CONFIG, or
# commit it as $(ROOT)/plugins.conf. A manifest is authoritative for WHICH
# directories are grafted, so an unrelated plugins/<dir> cannot change the
# build. It pins locations, not versions: a path carries no commit/tag/hash of
# the plugin sources, so content-level reproducibility still needs those
# revisions pinned elsewhere, and the default untracked plugins/* set stays
# machine-specific. `make plugin-manifest` prints the currently resolved set in
# exactly that form, so it can be saved as plugins.conf; `make check-plugins`
# validates the set and names any source that resolves outside this repository.
#
# The graft step below still links each plugin's wrapped*.c / *_private.h into
# the vendored tree and generates library_list_extra.h; the plugin's own
# `library_list.inc` stays inside the plugin directory. No dynamic-symbol
# visibility is restricted here: the wrapped library tables keep exporting the
# `my_*` / wrapper symbols box64's dlsym path may look up.
PLUGIN_CONFIG ?= $(ROOT)/plugins.conf
ifneq ($(wildcard $(PLUGIN_CONFIG)),)
PLUGIN_DIRS := $(foreach p,$(shell sed -n 's/^[^#=]*=[[:space:]]*//p' $(PLUGIN_CONFIG) | sed 's/[[:space:]]*$$//'),$(abspath $(p)))
PLUGIN_NAMES := $(shell sed -n 's/^[[:space:]]*\([^#=][^=]*[^[:space:]=]\)[[:space:]]*=.*$$/\1/p' $(PLUGIN_CONFIG))
else
PLUGIN_DIRS := $(wildcard $(ROOT)/plugins/*)
PLUGIN_NAMES := $(notdir $(PLUGIN_DIRS))
endif

# Sources grafted into the vendored box64 tree (symlinked only; box64 stays
# re-syncable). Explicit on purpose: src/core_shim.c and src/pt_mutex/* are C
# compiled by the Rust crate's build script and MUST NOT also be compiled into
# the engine archive (duplicate box64dl_core_* definitions).
#
# native_module_lib.c is the engine side of the native-module seam: it builds a
# runtime wrapped library from a registered descriptor. It is a normal ELFLOADER
# source (it needs box64's private library/bridge types); the vendored
# librarian/library.c reaches it through one patch call site.
# Select the target architecture independently from the build machine.  Set
# LIBBOX64DL_ARCH=x86_64 when cross-building on LoongArch (or use the native
# default on an x86_64 host).  The x64-only build is intentionally interpreter
# first: it is a useful gdb/debug artifact before a native CPU backend exists.
# On aarch64 the engine is the upstream ARM64 dynarec, selected with -DARM64=ON.
LIBBOX64_NATIVE_X64 ?= OFF
ifneq ($(filter x86_64 amd64,$(LIBBOX64DL_ARCH)),)
THUNK_ASM := $(ROOT)/src/thunk_x64.S
THUNK_SHIM_ASM := $(ROOT)/tests/test_thunk_shim.S
ARCH_CMAKE := -DLARCH64=OFF
NATIVE_CALL_SRC := $(if $(filter ON on 1,$(LIBBOX64_NATIVE_X64)),$(ROOT)/src/native_call_x64.S,)
NATIVE_CFLAGS := $(if $(NATIVE_CALL_SRC),-DBOX64_X64_NATIVE=1,)
else ifneq ($(filter aarch64 arm64,$(LIBBOX64DL_ARCH)),)
NATIVE_CALL_SRC :=
NATIVE_CFLAGS :=

THUNK_ASM := $(ROOT)/src/thunk_aarch64.S
THUNK_SHIM_ASM := $(ROOT)/tests/test_thunk_shim_aarch64.S
ARCH_CMAKE := -DARM64=ON -DLARCH64=OFF
else
NATIVE_CALL_SRC :=
NATIVE_CFLAGS :=

THUNK_ASM := $(ROOT)/src/thunk_la64.S
THUNK_SHIM_ASM := $(ROOT)/tests/test_thunk_shim.S
ARCH_CMAKE := -DLARCH64=ON
endif

# The process-wide signal-action pair (include/libbox64_signal_actions.h):
#
#   src/signal_actions.c  the action table, the four static kernel entries and
#       the public getter/setter. It includes NO engine header on purpose (the
#       engine-side seam is two symbols), so the same translation unit is also
#       buildable and runnable standalone — tests/test_signal_action_unit.c.
#   src/signal_guest.c    the guest worker: the guest-side lookup and handler
#       invocation the module hands a GUEST delivery to (patches/0021).
#
# Both are ELFLOADER sources, so they land in build{,-pic}/libbox64.a and the
# static test binaries and the shipped DSO get them with no extra link flag.
#
# GRAFT GATE. The module's engine half is the patch queue:
# patches/0020-signal-action-engine.patch installs the dispatcher from
# init_signal_helper and defines box64dl_engine_signal_hook;
# patches/0021-signal-guest-actions.patch wires the guest registration/query
# path and provides the guest-side worker the module hands a GUEST delivery to.
# Until BOTH are in the queue the module's engine-half symbols do not resolve,
# and the DSO link is `-Wl,-z,defs`, so grafting src/signal_actions.c early
# would break every shared-library gate for an unrelated reason.
#
# The gate therefore requires, all at once:
#   * EXACTLY ONE patches/0020*.patch, and
#   * EXACTLY ONE patches/0021*.patch, and
#   * both sources exist — src/signal_actions.c AND src/signal_guest.c.
#
# Counting per number, not the combined word count, is deliberate. The two
# patches are the two halves of one contract (0020 can name 0021's symbol and
# vice versa), so the gate must see one of each; a single prefix-count would
# also open on, say, two 0020 files and no 0021, or on a stray
# `0020-*.patch.bak`. Each wildcard is therefore counted on its own, and the
# combined list is kept only as the target's prerequisite name.
#
# The source half is not redundant. The patches compile the sources the gate
# would have grafted, so a queue that names 0020/0021 while one source has not
# landed yet would make CMake reference a missing file (or, with a wildcard,
# silently leave the module half-linked). Requiring the sources keeps the graft
# and the queue in step.
#
# WHAT A CLOSED GATE MEANS. The gate is all-or-nothing on artifacts and it only
# controls THIS build file: whether the module is grafted and whether the matrix
# target is in the default `test` set. It does NOT make any partial queue safe
# or validated. A partial configuration is NOT a supported layout — with the
# gate closed the honest statement is only "the matrix target is omitted from
# the default suite and the module is not grafted", not "a tree with one of the
# two patches works". Only the full feature (both patches AND both sources) is
# the intended configuration, and it opens by itself on the next `make`.
#
# The patch files and both sources are present in the tree, so this gate is OPEN,
# and the feature is not artifact-only any more: the queue was applied and
# reversed in a real live cycle and the new engine is built from that queue — the
# matrix below runs on it.
SIGNAL_ACTIONS_PATCH_0020 := $(wildcard $(ROOT)/patches/0020*.patch)
SIGNAL_ACTIONS_PATCH_0021 := $(wildcard $(ROOT)/patches/0021*.patch)
SIGNAL_ACTIONS_PATCHES := $(SIGNAL_ACTIONS_PATCH_0020) $(SIGNAL_ACTIONS_PATCH_0021)
SIGNAL_ACTIONS_SOURCES := $(wildcard $(ROOT)/src/signal_actions.c) \
	$(wildcard $(ROOT)/src/signal_guest.c)
SIGNAL_ACTIONS_READY := $(if $(filter 1,$(words $(SIGNAL_ACTIONS_PATCH_0020))),$(if $(filter 1,$(words $(SIGNAL_ACTIONS_PATCH_0021))),$(if $(filter 2,$(words $(SIGNAL_ACTIONS_SOURCES))),yes,)))
SIGNAL_ACTIONS_SRC := $(if $(SIGNAL_ACTIONS_READY),$(SIGNAL_ACTIONS_SOURCES))

# Conditional default-suite entry. When the gate is open, `make test` must
# actually exercise the feature, so the real-engine matrix joins the default
# regression. When it is closed the variable expands to nothing, so the matrix
# target is simply omitted from `test` and that recipe never runs. This is a
# statement about target membership only: it is NOT a claim that a tree with a
# partial queue builds or passes, and the default suite passing on such a tree is
# not asserted anywhere. This is the only conditional entry in `test`; no recipe
# is duplicated and no existing test is removed. (Named distinctly from
# TEST_SIGNAL_ACTIONS, which is the built binary path further down.)
SIGNAL_ACTIONS_TEST_TARGET := $(if $(SIGNAL_ACTIONS_READY),test-signal-actions)

VENDOR_SRC := $(ROOT)/src/diag.c $(ROOT)/src/libbox64.c \
	$(ROOT)/src/signal_policy.c $(ROOT)/src/signal_route.c \
	$(SIGNAL_ACTIONS_SRC) \
	$(ROOT)/src/thunk.c $(THUNK_ASM) $(NATIVE_CALL_SRC) \
	$(ROOT)/src/native_module.c $(ROOT)/src/native_module_lib.c

# The engine-owned signature module. It is a WRAPPEDS entry (not an ELFLOADER
# source) so box64's own rebuild_wrappers.py parses its _private.h and emits the
# wrapper functions the native-module seam can be asked for, and its .c builds
# the signature -> wrapper lookup from the very same list. Name and content are
# Node-agnostic: only SysV signature tokens, no guest symbols.
NATIVE_SIG_SRC := $(ROOT)/src/wrappedbox64dnative.c
NATIVE_SIG_PRIVATE := $(ROOT)/src/wrappedbox64dnative_private.h

CMAKE_COMMON := $(ARCH_CMAKE) -DCMAKE_BUILD_TYPE=RelWithDebInfo \
	-DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
	-DCMAKE_C_FLAGS="$(NATIVE_CFLAGS)" \
	-DNOLOADADDR=ON -DNO_CONF_INSTALL=ON -DNO_LIB_INSTALL=ON

# Dynarec does work inside a shared object. The old blocker was TLS_LE
# relocations, which only appear when box64 is built the default (non-PIC) way;
# at -fPIC the compiler emits TLS_LD instead and the link succeeds. Worth ~3x on
# guest-side hashing.
#
# Fallbacks: build with PIC_DYNAREC=OFF, or set BOX64_DYNAREC=0 at run time to
# drop back to the interpreter without rebuilding.
PIC_DYNAREC ?= ON

# -ftls-model is redundant under -fPIC on this toolchain; kept as a guard in
# case another one defaults to local-exec.
CMAKE_PIC := $(CMAKE_COMMON) -DLARCH64_DYNAREC=$(PIC_DYNAREC) \
	-DCMAKE_C_FLAGS="-fPIC -ftls-model=global-dynamic $(NATIVE_CFLAGS)" \
	-DCMAKE_ASM_FLAGS="-fPIC"

.PHONY: all static pic patch unpatch vendor check-vendor check-vendor-patches graft prune-stale-grafts plugins plugin-manifest check-plugins check-exports check-exports-negative test test-all test-detach-guest test-reset-stack test-call-arity test-call-plan test-call-fini test-call-unified bench-call-arity test-signal-policy test-signal-route test-guest-signal test-signal-library bench-signal-policy clean distclean rust rust-static \
	rust-pic cli check-consumer-dso-guest test-fini-quiescent \
	check-bridge-registration check-bridge-registration-unit bridge-seam-spike test-x64-native \
	test-signal-actions test-signal-action-unit

all: static pic

# All regression tests below run on a Linux host without Wine or a graphical
# desktop. Keep this as the default, fail-fast test entry point: each
# test-* prerequisite runs its own executable and a non-zero status stops make.
# The thunk pool is self-contained, so it tests without building box64.
# The export gate and its negative injection check are part of the default
# single-project gate; they check libbox64dl's own export table only.
#
# $(SIGNAL_ACTIONS_TEST_TARGET) is `test-signal-actions` ONLY when
# SIGNAL_ACTIONS_READY is yes (both patches and both sources present) and empty
# otherwise, so the real-engine signal-action matrix is part of the full
# regression exactly when the feature is actually in the build. When the gate is
# closed the matrix target is simply omitted from this list; that is a target-
# membership statement, not a claim about what a partial-queue tree can build or
# pass. The standalone `test-signal-action-unit` is unconditional: it needs
# neither patch, engine build nor guest fixture.
test: build/test_thunk test-detach-guest test-reset-stack test-call-arity test-call-plan test-call-fini test-fini-quiescent test-call-unified test-signal-policy test-signal-route test-guest-signal test-signal-library test-signal-action-unit $(SIGNAL_ACTIONS_TEST_TARGET) check-bridge-registration check-exports check-exports-negative check-no-node check-linkage check-engine-instances pkgconfig check-consumer check-consumer-dso-guest
	./build/test_thunk

# Complete regression-suite alias. There are no tests with extra Wine/display
# prerequisites in this tree, so test-all intentionally covers the same full
# set as test and is kept as a discoverable CI entry point.
test-all: test

# White-box check of the host-side detach repair (libbox64_ext.h). It needs
# the full engine — it reads the real x64emu_t fields — but no wine: it fakes
# the post-attach RSP/GS shape and asserts the switch, its rollback and its
# fail-closed path. Links exactly like a host embedder.
TEST_DETACH_GUEST := $(ROOT)/build/test_detach_guest

$(TEST_DETACH_GUEST): tests/test_detach_guest.c $(BUILD_STATIC)/libbox64.a $(RUST_STATIC)
	@mkdir -p $(dir $@)
	$(CC) -Og -g -std=c11 -Wall -Wextra -D_GNU_SOURCE \
		-Iinclude -I$(BOX64)/src/include -I$(BOX64)/src -o $@ tests/test_detach_guest.c \
		-Wl,--whole-archive $(BUILD_STATIC)/libbox64.a $(DYNAREC_STATIC) \
		-Wl,--no-whole-archive $(RUST_STATIC) \
		-lpthread -lm -ldl -lrt -lresolv -lutil -rdynamic

test-detach-guest: $(TEST_DETACH_GUEST)
	$(TEST_DETACH_GUEST)

# White-box check of the clean-guest-stack repair (libbox64_ext.h): the
# post-attach bridge-longjmp RSP is normalised onto the thread's own PE stack
# top while GS/TEB are kept. Same fake-TEB / real-engine link shape as the
# detach check above, so the emu fields are the real ones.
TEST_RESET_STACK := $(ROOT)/build/test_reset_stack

$(TEST_RESET_STACK): tests/test_reset_stack.c $(BUILD_STATIC)/libbox64.a $(RUST_STATIC)
	@mkdir -p $(dir $@)
	$(CC) -Og -g -std=c11 -Wall -Wextra -D_GNU_SOURCE \
		-Iinclude -I$(BOX64)/src/include -I$(BOX64)/src -o $@ tests/test_reset_stack.c \
		-Wl,--whole-archive $(BUILD_STATIC)/libbox64.a $(DYNAREC_STATIC) \
		-Wl,--no-whole-archive $(RUST_STATIC) \
		-lpthread -lm -ldl -lrt -lresolv -lutil -rdynamic

test-reset-stack: $(TEST_RESET_STACK)
	$(TEST_RESET_STACK)

# Rust facade. build.rs decides the flavor itself (feature/env), so these
# targets only ensure the matching box64 archive is up to date first. Each
# flavor keeps its own cargo target dir so both artifacts can coexist.
rust: rust-static
rust-static: $(BUILD_STATIC)/libbox64.a
	cd rust && CARGO_TARGET_DIR=target/static$(RUST_TARGET_SUFFIX) LIBBOX64_BUILD_DIR=$(BUILD_STATIC) LIBBOX64_NATIVE_X64=$(LIBBOX64_NATIVE_X64) \
		CC_x86_64_unknown_linux_gnu=$(CC) AR_x86_64_unknown_linux_gnu=$(AR) \
		CARGO_TARGET_X86_64_UNKNOWN_LINUX_GNU_LINKER=$(CC) cargo build $(RUST_TARGET_ARG)
rust-pic: $(BUILD_PIC)/libbox64.a
	cd rust && CARGO_TARGET_DIR=target/pic$(RUST_TARGET_SUFFIX) LIBBOX64_FLAVOR=pic LIBBOX64_BUILD_DIR=$(BUILD_PIC) LIBBOX64_NATIVE_X64=$(LIBBOX64_NATIVE_X64) \
		CC_x86_64_unknown_linux_gnu=$(CC) AR_x86_64_unknown_linux_gnu=$(AR) \
		CARGO_TARGET_X86_64_UNKNOWN_LINUX_GNU_LINKER=$(CC) cargo build $(RUST_TARGET_ARG)

# ---- packaged box64 CLI: the runtime/bin/box64 an app tree ships ----
# Thin main (cli/box64_main.c) over the static engine, linked with the same
# shape embedders use: whole-archive the engine + dynarec, then the Rust
# facade that resolves the facade symbols libbox64.c references. Wine's
# companion children (wineserver/wineboot) re-exec through this binary
# exactly like through a system box64.
CLI := $(ROOT)/build/bin/box64
RUST_STATIC := $(ROOT)/rust/target/static$(RUST_TARGET_SUFFIX)/$(if $(RUST_TARGET),$(RUST_TARGET)/,)debug/liblibbox64.a

SHARED := $(ROOT)/build/lib/libbox64dl.so
RUST_PIC := $(ROOT)/rust/target/pic$(RUST_TARGET_SUFFIX)/$(if $(RUST_TARGET),$(RUST_TARGET)/,)debug/liblibbox64.a

# ---- explicit native-module seam ---------------------------------------
# The engine side of the sibling provider's explicit native-module registration
# (patch 0010, include/libbox64_native_module.h). The gate has two halves, both
# in-repo:
#
#   test_native_module_registration  engine-free protocol check of the entry
#       (ABI version, descriptor validation, fixed capacity, exact soname
#        lookup). Compiles src/native_module.c with a plain host compiler.
#   test_bridge_wiring  the REAL engine: an unclaimed soname fails closed, and
#       a registered descriptor routes an otherwise-unlisted soname through
#       the wrapped-library path to a real host symbol.
#
# `bridge-seam-spike` is the cross-repository link check (it compiles the
# sibling provider tree's real seam sources against this engine). It is an
# explicit target, not part of `test`, because the engine's no-Node gate must
# stay provable on an engine-only checkout.
TEST_NATIVE_MODULE_REGISTRATION := $(ROOT)/build/test_native_module_registration
TEST_BRIDGE_WIRING := $(ROOT)/build/test_bridge_wiring

# Guest fixtures for the fail-closed regression (H1). Each is a real x86_64 ELF
# named exactly like a soname a descriptor claims, so the test can prove that a
# claimed-but-unbuildable soname does NOT silently load the guest file from the
# search path. Control and claimed images differ only in their constructor's
# marker file, so a load that should not happen is an unexpected marker.
SEAM_FIXTURE_DIR := $(ROOT)/build/guest/seam
GUEST_SEAM_CONTROL_SO := $(SEAM_FIXTURE_DIR)/libseam_control.so
GUEST_SEAM_CLAIMED_SO := $(SEAM_FIXTURE_DIR)/libseam_claimed.so

$(GUEST_SEAM_CONTROL_SO): tests/guest/collide_guest.c
	@mkdir -p $(dir $@)
	x86_64-linux-gnu-gcc -shared -fPIC -O1 -fno-stack-protector \
		-DMARKER_NAME='"seam_control_marker"' -o $@ $<

$(GUEST_SEAM_CLAIMED_SO): tests/guest/collide_guest.c
	@mkdir -p $(dir $@)
	x86_64-linux-gnu-gcc -shared -fPIC -O1 -fno-stack-protector \
		-DMARKER_NAME='"seam_claimed_marker"' -o $@ $<

$(TEST_NATIVE_MODULE_REGISTRATION): tests/test_native_module_registration.c \
		src/native_module.c include/libbox64_native_module.h
	@mkdir -p $(dir $@)
	$(CC) -Og -g -std=c11 -Wall -Wextra -D_GNU_SOURCE -Iinclude \
		-o $@ tests/test_native_module_registration.c src/native_module.c

check-bridge-registration-unit: $(TEST_NATIVE_MODULE_REGISTRATION)
	$(TEST_NATIVE_MODULE_REGISTRATION)

$(TEST_BRIDGE_WIRING): tests/test_bridge_wiring.c $(BUILD_STATIC)/libbox64.a \
		$(RUST_STATIC) include/libbox64_native_module.h \
		$(GUEST_SEAM_CONTROL_SO) $(GUEST_SEAM_CLAIMED_SO)
	@mkdir -p $(dir $@)
	$(CC) -Og -g -std=c11 -Wall -Wextra -D_GNU_SOURCE \
		-Iinclude -I$(BOX64)/src/include -I$(BOX64)/src -o $@ tests/test_bridge_wiring.c \
		-Wl,--whole-archive $(BUILD_STATIC)/libbox64.a $(DYNAREC_STATIC) \
		-Wl,--no-whole-archive $(RUST_STATIC) \
		-lpthread -lm -ldl -lrt -lresolv -lutil -rdynamic

check-bridge-registration: check-bridge-registration-unit $(TEST_BRIDGE_WIRING)
	$(TEST_BRIDGE_WIRING) $(SEAM_FIXTURE_DIR)

bridge-seam-spike: $(BUILD_STATIC)/libbox64.a $(RUST_STATIC)
	sh tools/bridge_seam_spike.sh

cli: $(CLI)

shared: $(SHARED)

# Public ABI gate: the engine DSO is the shipped export table. The gate is
# self-contained: it never looks at another repository, so an engine-only build
# can run it (tools/check_no_node.sh proves that separately).
#
# --expect-signal-actions is passed exactly when the build grafted
# src/signal_actions.c (see SIGNAL_ACTIONS_READY): then the two public
# signal-action entries must be dynamic, or the feature is unreachable from the
# shipped DSO. The default gate is unchanged for a build without the module.
check-exports: $(SHARED)
	python3 tools/check_exports.py --box64 $(SHARED) \
		$(if $(SIGNAL_ACTIONS_READY),--expect-signal-actions)

# Negative gate for the symbol check itself: every retired forward-call entry
# (and a Node-side name) injected into the observed set must be rejected. This
# proves the forbidden list is enforced instead of merely listed. It also
# injects an internal symbol and a Rust-mangled name to prove the "only
# box64_* + my* may be dynamic" class rule is enforced.
check-exports-negative: $(SHARED)
	@set -e; \
	fail=0; \
	for sym in box64_call_regs box64_call_marshaled box64_call_plan box64_call0 box64_lib_ready napi_create_function my_napi_create box64dl_core_boot_env box64dl_signal_action_get box64dl_signal_actions_init box64dl_signal_guest_deliver box64dl_engine_signal_hook _ZN5alloc3vec; do \
		if python3 tools/check_exports.py --box64 $(SHARED) --inject-symbol "$$sym" \
			>/dev/null 2>&1; then \
			echo "symbol-gate negative: FAIL: injected $$sym was accepted" >&2; \
			fail=1; \
		fi; \
	done; \
	if [ "$$fail" -ne 0 ]; then exit 1; fi; \
	echo "symbol-gate negative: PASS (retired, Node-side, shim and Rust-mangled symbols are rejected)"

# E1 gates: no Node build input, one Rust staticlib in the final link, one box64
# engine instance in the built DSO. Both gates inspect the built artifacts, so
# they depend on the DSO instead of silently skipping a missing build.
check-no-node: $(SHARED)
	sh tools/check_no_node.sh

check-linkage: $(SHARED)
	sh tools/check_linkage.sh

check-engine-instances: $(SHARED)
	sh tools/check_engine_instances.sh

# Downstream C consumer: public header + shipped DSO + the pkg-config file.
CONSUMER := $(ROOT)/build/consumer_smoke
$(CONSUMER): tests/consumer_smoke.c $(SHARED) $(PKGCONFIG)
	@mkdir -p $(dir $@)
	PKG_CONFIG_PATH=$(ROOT)/build/lib/pkgconfig \
		$(CC) -O2 -std=c11 -o $@ tests/consumer_smoke.c \
		$$(PKG_CONFIG_PATH=$(ROOT)/build/lib/pkgconfig pkg-config --cflags --libs libbox64dl) \
		-Wl,-rpath,$(ROOT)/build/lib

check-consumer: $(CONSUMER)
	$(CONSUMER)

# Real DT_NEEDED-libc guest + DSO-only consumer. The consumer links ONLY the
# shipped libbox64dl.so (pkg-config), so it is the one test that exercises the
# engine's own dlsym(my_context->box64lib, "my_*") lookup against the
# version-scripted DSO rather than the static test binaries' -rdynamic copy of
# every symbol. The guest uses printf/fopen/bsearch and carries a constructor
# and destructor, so a passing run observes libc bridging at load, call and
# fini (the destructor runs from endBox64()).
GUEST_LIBC_SO := $(ROOT)/build/guest/libc_guest.so

$(GUEST_LIBC_SO): tests/guest/libc_guest.c
	@mkdir -p $(dir $@)
	# Deliberately NOT -nostdlib: the image must carry DT_NEEDED libc.so.6.
	x86_64-linux-gnu-gcc -shared -fPIC -O1 -fno-stack-protector -o $@ $<

CONSUMER_DSO_GUEST := $(ROOT)/build/consumer_dso_guest

$(CONSUMER_DSO_GUEST): tests/consumer_dso_guest.c $(SHARED) $(PKGCONFIG) $(GUEST_LIBC_SO)
	@mkdir -p $(dir $@)
	PKG_CONFIG_PATH=$(ROOT)/build/lib/pkgconfig \
		$(CC) -O2 -std=c11 -Wall -Wextra -o $@ tests/consumer_dso_guest.c \
		$$(PKG_CONFIG_PATH=$(ROOT)/build/lib/pkgconfig pkg-config --cflags --libs libbox64dl) \
		-Wl,-rpath,$(ROOT)/build/lib

check-consumer-dso-guest: $(CONSUMER_DSO_GUEST)
	$(CONSUMER_DSO_GUEST) $(GUEST_LIBC_SO)

# Keep the PIC archive as an explicit prerequisite: recursing through
# rust-pic here would configure BUILD_PIC twice when shared is built with -j.
$(RUST_PIC): FORCE $(BUILD_PIC)/libbox64.a
	cd rust && CARGO_TARGET_DIR=target/pic LIBBOX64_FLAVOR=pic cargo build

# The exact command that produces the shipped engine DSO. `print-link-line` is
# the single source tools/check_linkage.sh reads, so the gate cannot drift from
# the real link.
define SHARED_LINK
$(CC) -fPIC -Iinclude -I$(BOX64)/src/include -shared -Wl,-soname,libbox64dl.so -Wl,-z,defs -Wl,--version-script=src/libbox64dl.map -o $(SHARED) cli/entry.c -Wl,--whole-archive $(BUILD_PIC)/libbox64.a $(DYNAREC_PIC) -Wl,--no-whole-archive $(RUST_PIC) -lpthread -lm -ldl -lrt -lresolv -lutil
endef
export SHARED_LINK

$(SHARED): cli/entry.c src/libbox64dl.map $(BUILD_PIC)/libbox64.a $(RUST_PIC)
	@mkdir -p $(dir $@)
	$(SHARED_LINK)

# Downstream C consumers: one header (include/) + one DSO + a pkg-config file.
PKGCONFIG := $(ROOT)/build/lib/pkgconfig/libbox64dl.pc

pkgconfig: $(SHARED) $(PKGCONFIG)

$(PKGCONFIG): $(SHARED)
	@mkdir -p $(dir $@)
	@printf '%s\n' \
		'prefix=$(ROOT)' \
		'libdir=$${prefix}/build/lib' \
		'includedir=$${prefix}/include' \
		'' \
		'Name: libbox64dl' \
		'Description: Embeddable box64 engine (x86_64 ELF loader and caller)' \
		'Version: 0.1.0' \
		'Libs: -L$${libdir} -lbox64dl' \
		'Cflags: -I$${includedir}' \
		> $@
	@echo "pkgconfig: wrote $@"

print-link-line:
	@printf '%s\n' "$$SHARED_LINK"

print-cflags:
	@echo -I$(ROOT)/include
print-ldflags:
	@echo -L$(ROOT)/build/lib -lbox64dl
print-backends:
	@echo "engine core (box64) + in-repo wrapped libgccs; no Node backend in this repo"

.PHONY: print-link-line print-cflags print-ldflags print-backends pkgconfig \
	check-no-node check-linkage check-engine-instances check-consumer

$(RUST_STATIC): FORCE
	$(MAKE) rust-static

FORCE:

$(CLI): cli/box64_main.c $(SHARED)
	@mkdir -p $(dir $@)
	$(CC) -O2 -std=c11 -D_GNU_SOURCE -I$(BOX64)/src/include -o $@ \
		cli/box64_main.c \
		-L$(dir $(SHARED)) -lbox64dl -Wl,-rpath,'$$ORIGIN/../lib:$$ORIGIN'

# Regression fixture for the unified call entries (include/libbox64.h):
# a real x86_64 guest .so called through box64_call_simple (via the test callN
# adapters in tests/call_compat.h), through the historical register-file
# semantics expressed as box64_call_desc() + FULL_REGS, and through a re-entrant
# host callback. The guest is built by the x86_64 cross toolchain and linked
# freestanding (-nostdlib) so it carries no guest libc dependency; box64 dlopens
# it like any other guest ELF.
GUEST_CALL_SO := $(ROOT)/build/guest/libcall_guest.so
TEST_CALL_ARITY := $(ROOT)/build/test_call_arity

$(GUEST_CALL_SO): tests/guest/call_guest.c tests/guest/struct_guest.c tests/guest/probe_guest.S
	@mkdir -p $(dir $@)
	x86_64-linux-gnu-gcc -shared -fPIC -O1 -nostdlib -fno-stack-protector \
		-o $@ tests/guest/call_guest.c tests/guest/struct_guest.c \
		tests/guest/probe_guest.S

# Dedicated real-signal fixture: a guest-installed SIGUSR1 handler rewrites its
# ucontext RIP, driving box64's my_sigactionhandler_oldcode -> siglongjmp resume
# (route-independent, so it works on a GS=0 Linux guest with no external patch).
GUEST_SIG_SO := $(ROOT)/build/guest/libsig_guest.so

$(GUEST_SIG_SO): tests/guest/sig_guest.c
	@mkdir -p $(dir $@)
	x86_64-linux-gnu-gcc -shared -fPIC -O1 -nostdlib -fno-stack-protector \
		-o $@ tests/guest/sig_guest.c

# Same behavioural fixture under different exported names: a second, distinct
# guest image so the per-library policy tests have two independent libraries.
GUEST_SIG2_SO := $(ROOT)/build/guest/libsig_guest2.so

$(GUEST_SIG2_SO): tests/guest/sig_guest2.c tests/guest/sig_guest.c
	@mkdir -p $(dir $@)
	x86_64-linux-gnu-gcc -shared -fPIC -O1 -nostdlib -fno-stack-protector \
		-o $@ tests/guest/sig_guest2.c

# A guest image with a real DT_NEEDED dependency on libsig_guest2.so, for the
# dependency-unload policy-range test.
GUEST_DEP_SO := $(ROOT)/build/guest/libdep_guest.so

$(GUEST_DEP_SO): tests/guest/dep_guest.c $(GUEST_SIG2_SO)
	@mkdir -p $(dir $@)
	x86_64-linux-gnu-gcc -shared -fPIC -O1 -nostdlib -fno-stack-protector \
		-o $@ tests/guest/dep_guest.c -L$(dir $(GUEST_SIG2_SO)) -lsig_guest2 \
		-Wl,--no-as-needed -Wl,-rpath,'$$ORIGIN'

$(TEST_CALL_ARITY): tests/test_call_arity.c $(GUEST_CALL_SO) $(BUILD_STATIC)/libbox64.a
	@mkdir -p $(dir $@)
	$(CC) -Og -g -std=c11 -Wall -Wextra -D_GNU_SOURCE \
		-Iinclude -I$(BOX64)/src/include -I$(BOX64)/src -o $@ tests/test_call_arity.c \
		-Wl,--whole-archive $(BUILD_STATIC)/libbox64.a $(DYNAREC_STATIC) \
		-Wl,--no-whole-archive $(RUST_STATIC) \
		-lpthread -lm -ldl -lrt -lresolv -lutil -rdynamic

test-call-arity: $(TEST_CALL_ARITY)
	@echo "== call-arity: dynarec =="
	$(NATIVE_RUN_ENV) $(TEST_CALL_ARITY) $(GUEST_CALL_SO)
	@echo "== call-arity: interpreter (BOX64_DYNAREC=0) =="
	BOX64_DYNAREC=0 $(NATIVE_RUN_ENV) $(TEST_CALL_ARITY) $(GUEST_CALL_SO)

# Microbenchmark for the call path (old register-file entries vs the
# fixed-arity fast entries, diagnostics off vs on). Prints ns/call to stderr.
# Run with `>/dev/null` so the diagnostics-on phase does not flood the log.
TEST_BENCH_ARITY := $(ROOT)/build/bench_call_arity

$(TEST_BENCH_ARITY): tests/bench_call_arity.c $(GUEST_CALL_SO) $(BUILD_STATIC)/libbox64.a
	@mkdir -p $(dir $@)
	$(CC) -O2 -g -std=c11 -Wall -Wextra -D_GNU_SOURCE \
		-Iinclude -I$(BOX64)/src/include -I$(BOX64)/src -o $@ tests/bench_call_arity.c \
		-Wl,--whole-archive $(BUILD_STATIC)/libbox64.a $(DYNAREC_STATIC) \
		-Wl,--no-whole-archive $(RUST_STATIC) \
		-lpthread -lm -ldl -lrt -lresolv -lutil -rdynamic

bench-call-arity: $(TEST_BENCH_ARITY)
	$(TEST_BENCH_ARITY) $(GUEST_CALL_SO) 200000

# Descriptor seam (box64_call_desc): register probe + x86_64-compiler
# oracle for aggregate placement (including the two-eightbyte s3f case).
TEST_CALL_PLAN := $(ROOT)/build/test_call_plan

$(TEST_CALL_PLAN): tests/test_call_plan.c $(GUEST_CALL_SO) $(BUILD_STATIC)/libbox64.a
	@mkdir -p $(dir $@)
	$(CC) -Og -g -std=c11 -Wall -Wextra -D_GNU_SOURCE \
		-Iinclude -I$(BOX64)/src/include -I$(BOX64)/src -o $@ tests/test_call_plan.c \
		-Wl,--whole-archive $(BUILD_STATIC)/libbox64.a $(DYNAREC_STATIC) \
		-Wl,--no-whole-archive $(RUST_STATIC) \
		-lpthread -lm -ldl -lrt -lresolv -lutil -rdynamic

test-call-plan: $(TEST_CALL_PLAN)
	$(TEST_CALL_PLAN) $(GUEST_CALL_SO)

# Unified status/descriptor seam regression: validation, physical sparse masks,
# complete SSE values, stack rounding/tail clearing, simple arities and reentry.
TEST_CALL_UNIFIED := $(ROOT)/build/test_call_unified

$(TEST_CALL_UNIFIED): tests/test_call_unified.c $(GUEST_CALL_SO) $(BUILD_STATIC)/libbox64.a
	@mkdir -p $(dir $@)
	$(CC) -Og -g -std=c11 -Wall -Wextra -D_GNU_SOURCE \
		-Iinclude -I$(BOX64)/src/include -I$(BOX64)/src -o $@ tests/test_call_unified.c \
		-Wl,--whole-archive $(BUILD_STATIC)/libbox64.a $(DYNAREC_STATIC) \
		-Wl,--no-whole-archive $(RUST_STATIC) \
		-lpthread -lm -ldl -lrt -lresolv -lutil -rdynamic

test-call-unified: $(TEST_CALL_UNIFIED)
	$(TEST_CALL_UNIFIED) $(GUEST_CALL_SO)

# Lifetime guard: calls after box64_lib_fini() are refused, calls before run.
TEST_CALL_FINI := $(ROOT)/build/test_call_fini

$(TEST_CALL_FINI): tests/test_call_fini.c $(GUEST_CALL_SO) $(BUILD_STATIC)/libbox64.a
	@mkdir -p $(dir $@)
	$(CC) -Og -g -std=c11 -Wall -Wextra -D_GNU_SOURCE \
		-Iinclude -I$(BOX64)/src/include -I$(BOX64)/src -o $@ tests/test_call_fini.c \
		-Wl,--whole-archive $(BUILD_STATIC)/libbox64.a $(DYNAREC_STATIC) \
		-Wl,--no-whole-archive $(RUST_STATIC) \
		-lpthread -lm -ldl -lrt -lresolv -lutil -rdynamic

test-call-fini: $(TEST_CALL_FINI)
	$(TEST_CALL_FINI) $(GUEST_CALL_SO)

# Direct CPU x86_64 regression aggregate. These tests exercise the common
# descriptor, callback, stack, threading and lifecycle contracts; tests that
# specifically assert DynaCall signal-frame internals remain in `test`.
test-x64-native:
	$(MAKE) LIBBOX64DL_ARCH=x86_64 LIBBOX64_NATIVE_X64=ON test-call-arity
	$(MAKE) LIBBOX64DL_ARCH=x86_64 LIBBOX64_NATIVE_X64=ON test-call-plan
	$(MAKE) LIBBOX64DL_ARCH=x86_64 LIBBOX64_NATIVE_X64=ON test-call-unified
	$(MAKE) LIBBOX64DL_ARCH=x86_64 LIBBOX64_NATIVE_X64=ON test-call-fini
	$(MAKE) LIBBOX64DL_ARCH=x86_64 LIBBOX64_NATIVE_X64=ON test-fini-quiescent

# Sequential teardown regression: finish the worker's guest call and callback
# reentry, then fini; later calls must be refused by the terminal phase.
TEST_FINI_QUIESCENT := $(ROOT)/build/test_fini_quiescent

$(TEST_FINI_QUIESCENT): tests/test_fini_quiescent.c $(GUEST_CALL_SO) $(BUILD_STATIC)/libbox64.a
	@mkdir -p $(dir $@)
	$(CC) -Og -g -std=c11 -Wall -Wextra -D_GNU_SOURCE \
		-Iinclude -I$(BOX64)/src/include -I$(BOX64)/src -o $@ tests/test_fini_quiescent.c \
		-Wl,--whole-archive $(BUILD_STATIC)/libbox64.a $(DYNAREC_STATIC) \
		-Wl,--no-whole-archive $(RUST_STATIC) \
		-lpthread -lm -ldl -lrt -lresolv -lutil -rdynamic

test-fini-quiescent: $(TEST_FINI_QUIESCENT)
	$(TEST_FINI_QUIESCENT) $(GUEST_CALL_SO)

# Signal-policy regression (patches/0005): the per-call signal-mask syscall,
# the per-frame mixed-mode restore and the policy selection, driven by REAL
# signals. The global default is FAST now, so every COMPAT case selects it
# explicitly. The global default is immutable after init, so each case is a
# fresh process.
TEST_SIGNAL_POLICY := $(ROOT)/build/test_signal_policy

$(TEST_SIGNAL_POLICY): tests/test_signal_policy.c $(GUEST_CALL_SO) $(BUILD_STATIC)/libbox64.a
	@mkdir -p $(dir $@)
	$(CC) -Og -g -std=c11 -Wall -Wextra -D_GNU_SOURCE \
		-Iinclude -I$(BOX64)/src/include -I$(BOX64)/src -o $@ tests/test_signal_policy.c \
		-Wl,--whole-archive $(BUILD_STATIC)/libbox64.a $(DYNAREC_STATIC) \
		-Wl,--no-whole-archive $(RUST_STATIC) \
		-lpthread -lm -ldl -lrt -lresolv -lutil -rdynamic

test-signal-policy: $(TEST_SIGNAL_POLICY)
	@echo "== signal-policy: FAST (default) =="
	$(TEST_SIGNAL_POLICY) $(GUEST_CALL_SO) default
	$(TEST_SIGNAL_POLICY) $(GUEST_CALL_SO) helper-default
	BOX64DL_SIGNAL_STATS=1 $(TEST_SIGNAL_POLICY) $(GUEST_CALL_SO) default
	@echo "== signal-policy: FAST (env) =="
	BOX64DL_SIGNAL_POLICY=fast $(TEST_SIGNAL_POLICY) $(GUEST_CALL_SO) fast-env
	BOX64DL_SIGNAL_POLICY=fast $(TEST_SIGNAL_POLICY) $(GUEST_CALL_SO) helper-fast
	BOX64DL_SIGNAL_POLICY=fast BOX64DL_SIGNAL_STATS=1 $(TEST_SIGNAL_POLICY) $(GUEST_CALL_SO) fast-env
	@echo "== signal-policy: COMPAT (explicit) =="
	BOX64DL_SIGNAL_POLICY=compat $(TEST_SIGNAL_POLICY) $(GUEST_CALL_SO) compat-env
	BOX64DL_SIGNAL_POLICY=compat $(TEST_SIGNAL_POLICY) $(GUEST_CALL_SO) helper-compat
	BOX64DL_SIGNAL_POLICY=compat BOX64DL_SIGNAL_STATS=1 $(TEST_SIGNAL_POLICY) $(GUEST_CALL_SO) compat-env
	@echo "== signal-policy: FAST (api) =="
	$(TEST_SIGNAL_POLICY) $(GUEST_CALL_SO) api-fast
	$(TEST_SIGNAL_POLICY) $(GUEST_CALL_SO) api-compat
	@echo "== signal-policy: FAST threads / reentry =="
	$(TEST_SIGNAL_POLICY) $(GUEST_CALL_SO) threads
	$(TEST_SIGNAL_POLICY) $(GUEST_CALL_SO) reenter
	@echo "== signal-policy: fini restores the host handler (workers joined) =="
	$(TEST_SIGNAL_POLICY) $(GUEST_CALL_SO) fini-workers
	BOX64DL_SIGNAL_POLICY=compat $(TEST_SIGNAL_POLICY) $(GUEST_CALL_SO) fini-workers
	@echo "== signal-policy: fini does not clobber a post-init host action =="
	$(TEST_SIGNAL_POLICY) $(GUEST_CALL_SO) fini-host-changed
	@echo "== signal-policy: guest fatal handler does not defeat fini restore =="
	$(TEST_SIGNAL_POLICY) $(GUEST_CALL_SO) fini-guest-fatal
	BOX64DL_SIGNAL_POLICY=compat $(TEST_SIGNAL_POLICY) $(GUEST_CALL_SO) fini-guest-fatal
	@echo "== signal-policy: interpreter (BOX64_DYNAREC=0) =="
	BOX64_DYNAREC=0 $(TEST_SIGNAL_POLICY) $(GUEST_CALL_SO) default
	BOX64_DYNAREC=0 BOX64DL_SIGNAL_POLICY=compat $(TEST_SIGNAL_POLICY) $(GUEST_CALL_SO) compat-env
	@echo "== signal-policy: refusals (fail-fast) =="
	BOX64DL_SIGNAL_POLICY=none $(TEST_SIGNAL_POLICY) $(GUEST_CALL_SO) env-none
	BOX64DL_SIGNAL_POLICY=bogus $(TEST_SIGNAL_POLICY) $(GUEST_CALL_SO) env-bogus
	$(TEST_SIGNAL_POLICY) $(GUEST_CALL_SO) set-bad

# Fault-routing regression (src/signal_route.c). SEPARATE concern from the mask
# policy; driven by real signals (host raise, host fault inside a bridge
# callback). NOTE: src/signal_route.c is now the retired GS-free compatibility
# shim — it returns 0 and classifies nothing — and patches/0020 removes the call
# site from my_box64signalhandler(). This target is kept as the host-side
# regression for the pre-0020 forwarded path; engine-signal delivery is decided
# by the action module and covered by test-signal-actions. Whether these host-
# side expectations still hold once 0020 is applied is a 0020/acceptance
# question, not something asserted here.
TEST_SIGNAL_ROUTE := $(ROOT)/build/test_signal_route

$(TEST_SIGNAL_ROUTE): tests/test_signal_route.c $(GUEST_CALL_SO) $(BUILD_STATIC)/libbox64.a
	@mkdir -p $(dir $@)
	$(CC) -Og -g -std=c11 -Wall -Wextra -D_GNU_SOURCE \
		-Iinclude -I$(BOX64)/src/include -I$(BOX64)/src -o $@ tests/test_signal_route.c \
		-Wl,--whole-archive $(BUILD_STATIC)/libbox64.a $(DYNAREC_STATIC) \
		-Wl,--no-whole-archive $(RUST_STATIC) \
		-lpthread -lm -ldl -lrt -lresolv -lutil -rdynamic

test-signal-route: $(TEST_SIGNAL_ROUTE)
	@echo "== signal-route: dynarec =="
	$(TEST_SIGNAL_ROUTE) $(GUEST_CALL_SO)

# Real guest-signal restore probe: a guest SIGUSR1 handler with a non-empty
# sa_mask (no SA_NODEFER) rewrites its ucontext RIP, so box64 resumes through
# siglongjmp. Route-independent => runs on a GS=0 guest with no external patch.
TEST_GUEST_SIGNAL := $(ROOT)/build/test_guest_signal

$(TEST_GUEST_SIGNAL): tests/test_guest_signal.c $(GUEST_SIG_SO) $(BUILD_STATIC)/libbox64.a
	@mkdir -p $(dir $@)
	$(CC) -Og -g -std=c11 -Wall -Wextra -D_GNU_SOURCE \
		-Iinclude -I$(BOX64)/src/include -I$(BOX64)/src -o $@ tests/test_guest_signal.c \
		-Wl,--whole-archive $(BUILD_STATIC)/libbox64.a $(DYNAREC_STATIC) \
		-Wl,--no-whole-archive $(RUST_STATIC) \
		-lpthread -lm -ldl -lrt -lresolv -lutil -rdynamic

test-guest-signal: $(TEST_GUEST_SIGNAL)
	@echo "== guest-signal: FAST (default) dynarec =="
	$(TEST_GUEST_SIGNAL) $(GUEST_SIG_SO)
	@echo "== guest-signal: COMPAT (explicit) dynarec =="
	BOX64DL_SIGNAL_POLICY=compat $(TEST_GUEST_SIGNAL) $(GUEST_SIG_SO)
	@echo "== guest-signal: FAST interpreter =="
	BOX64_DYNAREC=0 $(TEST_GUEST_SIGNAL) $(GUEST_SIG_SO)
	@echo "== guest-signal: COMPAT interpreter =="
	BOX64_DYNAREC=0 BOX64DL_SIGNAL_POLICY=compat $(TEST_GUEST_SIGNAL) $(GUEST_SIG_SO)

# Per-library policy + environment rules: two distinct guest .so images, an
# explicit per-handle override, an environment basename/full-path rule and the
# precedence between them, plus invalid handles and unload/reload. See
# tests/test_signal_library.c and include/libbox64.h.
TEST_SIGNAL_LIBRARY := $(ROOT)/build/test_signal_library

$(TEST_SIGNAL_LIBRARY): tests/test_signal_library.c $(GUEST_CALL_SO) $(GUEST_SIG2_SO) $(GUEST_DEP_SO) $(BUILD_STATIC)/libbox64.a
	@mkdir -p $(dir $@)
	$(CC) -Og -g -std=c11 -Wall -Wextra -D_GNU_SOURCE \
		-Iinclude -I$(BOX64)/src/include -I$(BOX64)/src -o $@ tests/test_signal_library.c \
		-Wl,--whole-archive $(BUILD_STATIC)/libbox64.a $(DYNAREC_STATIC) \
		-Wl,--no-whole-archive $(RUST_STATIC) \
		-lpthread -lm -ldl -lrt -lresolv -lutil -rdynamic

test-signal-library: $(TEST_SIGNAL_LIBRARY)
	@echo "== signal-library: explicit > env > default =="
	BOX64DL_SIGNAL_LIBRARY_RULES='libcall_guest.so=compat' $(TEST_SIGNAL_LIBRARY) $(GUEST_CALL_SO) $(GUEST_SIG2_SO) precedence
	@echo "== signal-library: env basename =="
	BOX64DL_SIGNAL_LIBRARY_RULES='libsig_guest2.so=compat' $(TEST_SIGNAL_LIBRARY) $(GUEST_CALL_SO) $(GUEST_SIG2_SO) env-basename
	@echo "== signal-library: env full path =="
	BOX64DL_SIGNAL_LIBRARY_RULES="$(GUEST_CALL_SO)=compat" $(TEST_SIGNAL_LIBRARY) $(GUEST_CALL_SO) $(GUEST_SIG2_SO) env-fullpath
	@echo "== signal-library: no substring false match =="
	BOX64DL_SIGNAL_LIBRARY_RULES='call_guest.so=compat' $(TEST_SIGNAL_LIBRARY) $(GUEST_CALL_SO) $(GUEST_SIG2_SO) env-nosubstring
	@echo "== signal-library: invalid handle / no-elf refusal =="
	$(TEST_SIGNAL_LIBRARY) $(GUEST_CALL_SO) $(GUEST_SIG2_SO) invalid-handle
	@echo "== signal-library: unload/reload does not leak a policy =="
	$(TEST_SIGNAL_LIBRARY) $(GUEST_CALL_SO) $(GUEST_SIG2_SO) unload-reload
	@echo "== signal-library: multithread per-library scopes =="
	$(TEST_SIGNAL_LIBRARY) $(GUEST_CALL_SO) $(GUEST_SIG2_SO) threads
	@echo "== signal-library: invalid config refused =="
	$(TEST_SIGNAL_LIBRARY) $(GUEST_CALL_SO) $(GUEST_SIG2_SO) invalid-config
	@echo "== signal-library: override capacity refused (no silent FAST) =="
	$(TEST_SIGNAL_LIBRARY) $(GUEST_CALL_SO) $(GUEST_SIG2_SO) overflow
	@echo "== signal-library: dependency unload clears its policy range =="
	BOX64DL_SIGNAL_LIBRARY_RULES='libsig_guest2.so=compat' $(TEST_SIGNAL_LIBRARY) $(GUEST_CALL_SO) $(GUEST_SIG2_SO) dep-unload $(GUEST_DEP_SO)
	@echo "== signal-library: foreign-loader range/rule seam =="
	BOX64DL_SIGNAL_LIBRARY_RULES='libsig_guest2.so=compat' $(TEST_SIGNAL_LIBRARY) $(GUEST_CALL_SO) $(GUEST_SIG2_SO) foreign
	@echo "== signal-library: per-call arm counter switch =="
	$(TEST_SIGNAL_LIBRARY) $(GUEST_CALL_SO) $(GUEST_SIG2_SO) counter
	@echo "== signal-library: malformed rule fails init =="
	BOX64DL_SIGNAL_LIBRARY_RULES='libcall_guest.so=compatx' $(TEST_SIGNAL_LIBRARY) $(GUEST_CALL_SO) $(GUEST_SIG2_SO) badrule
	@echo "== signal-library: env rule covers constructors =="
	BOX64DL_SIGNAL_LIBRARY_RULES='libcall_guest.so=compat' $(TEST_SIGNAL_LIBRARY) $(GUEST_CALL_SO) $(GUEST_SIG2_SO) constructors
	BOX64DL_SIGNAL_LIBRARY_RULES='libcall_guest.so=fast' $(TEST_SIGNAL_LIBRARY) $(GUEST_CALL_SO) $(GUEST_SIG2_SO) constructors
	@echo "== signal-library: mixed-mode nested real SIGUSR1 (both directions) =="
	$(TEST_SIGNAL_LIBRARY) $(GUEST_CALL_SO) $(GUEST_SIG2_SO) mixed
	BOX64_DYNAREC=0 $(TEST_SIGNAL_LIBRARY) $(GUEST_CALL_SO) $(GUEST_SIG2_SO) mixed

# ---- process-wide signal actions (include/libbox64_signal_actions.h) ----
# One logical action per host signal, shared by the native host and the guest:
# the app's pre-init native disposition is the initial value, a guest
# rt_sigaction(2) replaces it, box64_signal_action_set_native() replaces it
# again, and either side reads the other's action back with
# box64_signal_action_get(). See include/libbox64_signal_actions.h.
#
# Two build layers, deliberately separated:
#
#   test-signal-action-unit   the core module on its own. src/signal_actions.c
#       includes no engine header; it reaches the engine through exactly two
#       symbols, which the test defines as doubles (plus -Wl,--wrap=sigaction to
#       watch the kernel dispositions it installs). No patch apply, no engine
#       build, no guest fixture and no third stub dependency, so it runs both
#       before and after the engine half lands. 15/15; it is part of `make test`.
#
#   test-signal-actions       the real engine. It needs src/signal_actions.c
#       grafted into build/libbox64.a, which needs BOTH patches/0020 (init
#       install + engine hook) and patches/0021 (guest worker). Gated on them:
#       before they exist the target reports what is missing instead of a link
#       error about two unresolved symbols.
GUEST_SIGNAL_ACTIONS_SO := $(ROOT)/build/guest/libsignal_actions_guest.so

# The fixture is a freestanding x86_64 image: its own syscall stubs and its own
# memcpy/memset, so -fno-builtin -ffreestanding are load-bearing. Without them
# GCC is free to emit a call to libc's memcpy from those very functions, which
# leaves an undefined symbol in a -nostdlib image.
$(GUEST_SIGNAL_ACTIONS_SO): tests/guest/signal_actions_guest.c
	@mkdir -p $(dir $@)
	x86_64-linux-gnu-gcc -shared -fPIC -O1 -nostdlib -fno-stack-protector \
		-fno-builtin -ffreestanding -Wall -Wextra -o $@ $<

TEST_SIGNAL_ACTION_UNIT := $(ROOT)/build/test_signal_action_unit

$(TEST_SIGNAL_ACTION_UNIT): tests/test_signal_action_unit.c src/signal_actions.c \
		include/libbox64_signal_actions.h
	@mkdir -p $(dir $@)
	$(CC) -std=c11 -Wall -Wextra -Werror -Iinclude -pthread \
		-Wl,--wrap=sigaction -o $@ tests/test_signal_action_unit.c src/signal_actions.c

test-signal-action-unit: $(TEST_SIGNAL_ACTION_UNIT)
	$(TEST_SIGNAL_ACTION_UNIT)

TEST_SIGNAL_ACTIONS := $(ROOT)/build/test_signal_actions

$(TEST_SIGNAL_ACTIONS): tests/test_signal_actions.c $(GUEST_SIGNAL_ACTIONS_SO) \
		$(BUILD_STATIC)/libbox64.a $(SIGNAL_ACTIONS_PATCHES)
	@mkdir -p $(dir $@)
	$(CC) -Og -g -std=c11 -Wall -Wextra -D_GNU_SOURCE \
		-Iinclude -I$(BOX64)/src/include -I$(BOX64)/src -o $@ tests/test_signal_actions.c \
		-Wl,--whole-archive $(BUILD_STATIC)/libbox64.a $(DYNAREC_STATIC) \
		-Wl,--no-whole-archive $(RUST_STATIC) \
		-lpthread -lm -ldl -lrt -lresolv -lutil -rdynamic

# The matrix is read as DATA from tests/signal-actions-cases.list, which is the
# normative list. Fields one to three of every line that does not start with
# whitespace are <case> <policy> <modes>; a line that DOES start with whitespace
# is a wrapped prose continuation of the fourth ("what it proves") column and is
# skipped. A hand-transcribed target drifts from the list; this one cannot.
#
# Cartesian expansion, one fresh process per run because the action table and
# the mask-policy default are process-wide and one-shot:
#   both     BOX64_DYNAREC=1 and =0, crossed with the policy column
#   dynarec  JIT only (smc is a self-modifying-code repair: no interpreter case)
#   single   once, both variables unset (not-init asserts ENOSYS BEFORE the
#            engine is up, so it cannot run under a policy)
# A '-' policy column leaves BOX64DL_SIGNAL_POLICY unset, so the engine default
# is what the case exercises. The allocation-sensitive cases run under
# MALLOC_PERTURB_=165 so a stale read or use-after-free shows as garbage instead
# of silence.
#
# Fail-fast: the matrix is one shell with `set -e`, so the first failing case
# aborts make. Cases that fork children, join a bounded worker or race real
# deliveries get 60 s; the rest get 30 s (already far above their real runtime).
SA_CASES := $(ROOT)/tests/signal-actions-cases.list
SA_SLOW := concurrent resethand-race fatal pagefix native-fault native-c-fault smc chld longjmp uc-mask

test-signal-actions:
	@set -e; \
	if [ -z "$(SIGNAL_ACTIONS_READY)" ]; then \
		echo "test-signal-actions: engine half not in the queue yet." >&2; \
		echo "  need exactly one patches/0020*.patch (init install +" >&2; \
		echo "  box64dl_engine_signal_hook) and exactly one patches/0021*.patch" >&2; \
		echo "  (guest registration path + guest worker), plus src/signal_actions.c" >&2; \
		echo "  and src/signal_guest.c. found 0020: $(notdir $(SIGNAL_ACTIONS_PATCH_0020))" >&2; \
		echo "  found 0021: $(notdir $(SIGNAL_ACTIONS_PATCH_0021))" >&2; \
		echo "  the core module itself is testable now: make test-signal-action-unit" >&2; \
		exit 1; \
	fi; \
	test -f "$(SA_CASES)" || { echo "test-signal-actions: no $(SA_CASES)" >&2; exit 1; }; \
	$(MAKE) --no-print-directory $(TEST_SIGNAL_ACTIONS) $(GUEST_SIGNAL_ACTIONS_SO); \
	n=0; \
	echo "== signal-actions: matrix from tests/signal-actions-cases.list =="; \
	while IFS= read -r line; do \
		case "$$line" in ''|\#*|[[:space:]]*) continue ;; esac; \
		set -- $$line; \
		c=$$1; pol=$$2; modes=$$3; \
		[ -n "$$c" ] && [ -n "$$modes" ] || continue; \
		case "$$modes" in \
			both) set -- 1 0 ;; \
			dynarec) set -- 1 ;; \
			*) set -- - ;; \
		esac; \
		for dy in "$$@"; do \
			env=""; \
			[ "$$dy" = "-" ] || env="BOX64_DYNAREC=$$dy"; \
			[ "$$pol" = "-" ] || env="$$env BOX64DL_SIGNAL_POLICY=$$pol"; \
			case "$$c" in concurrent|resethand-race) env="MALLOC_PERTURB_=165 $$env" ;; esac; \
			t=30; \
			case " $(SA_SLOW) " in *" $$c "*) t=60 ;; esac; \
			n=$$((n+1)); \
			echo "== signal-actions: $$c (dynarec=$$dy policy=$$pol timeout=$${t}s) =="; \
			timeout $$t env $$env $(TEST_SIGNAL_ACTIONS) $(GUEST_SIGNAL_ACTIONS_SO) $$c \
				|| { echo "signal-actions: FAIL $$c (dynarec=$$dy policy=$$pol)" >&2; exit 1; }; \
		done; \
	done < "$(SA_CASES)"; \
	echo "== signal-actions: $$n process(es) passed =="

# Per-call signal-policy microbenchmark (the "policy cases" of the bench suite;
# the fixed-arity call-path numbers live in bench_call_arity). Fresh process per
# policy because the global default is immutable after init; numbers on stderr,
# library traces on stdout.
TEST_BENCH_SIGNAL := $(ROOT)/build/bench_signal_policy

$(TEST_BENCH_SIGNAL): tests/bench_signal_policy.c $(GUEST_CALL_SO) $(BUILD_STATIC)/libbox64.a
	@mkdir -p $(dir $@)
	$(CC) -O2 -g -std=c11 -Wall -Wextra -D_GNU_SOURCE \
		-Iinclude -I$(BOX64)/src/include -I$(BOX64)/src -o $@ tests/bench_signal_policy.c \
		-Wl,--whole-archive $(BUILD_STATIC)/libbox64.a $(DYNAREC_STATIC) \
		-Wl,--no-whole-archive $(RUST_STATIC) \
		-lpthread -lm -ldl -lrt -lresolv -lutil -rdynamic

bench-signal-policy: $(TEST_BENCH_SIGNAL)
	@mkdir -p build/test-results
	@echo "== bench signal-policy: global default (FAST) =="
	$(TEST_BENCH_SIGNAL) $(GUEST_CALL_SO) 200000 >/dev/null
	@echo "== bench signal-policy: global FAST + per-library COMPAT override =="
	$(TEST_BENCH_SIGNAL) $(GUEST_CALL_SO) 200000 --library-policy compat \
		--json build/test-results/performance.json >/dev/null

build/test_thunk: tests/test_thunk.c $(THUNK_SHIM_ASM) src/thunk.c $(THUNK_ASM) src/diag.c
	@mkdir -p build
	$(CC) -Og -g -std=c11 -Wall -Wextra -Iinclude -o $@ $^ -lpthread

static: $(BUILD_STATIC)/libbox64.a
pic: $(BUILD_PIC)/libbox64.a

# Materialise the engine submodule. Needs network on a fresh clone; a machine
# that already has .git/modules/vendor/box64 just checks out the pinned commit.
#
# On a network-less machine, point the public URL at a local mirror:
#   make vendor BOX64_GIT_OPTS='-c protocol.file.allow=always \
#     -c url."/path/to/box64".insteadOf=https://github.com/ptitSeb/box64.git'
BOX64_GIT_OPTS ?=
vendor:
	git -C "$(ROOT)" $(BOX64_GIT_OPTS) submodule update --init --recursive vendor/box64

# Fail before patching/building when the engine submodule is missing or is not
# the pinned commit. A wrong engine would otherwise surface as a patch that
# "cannot apply" or a compiler error far from the real cause.
check-vendor:
	@test -f "$(BOX64)/CMakeLists.txt" \
		|| { echo "check-vendor: $(BOX64) is empty; run 'make vendor' (git submodule update --init vendor/box64)" >&2; exit 1; }
	@cur=$$(git -C "$(BOX64)" rev-parse HEAD 2>/dev/null); \
	if [ "$$cur" != "$(BOX64_REF)" ]; then \
		echo "check-vendor: vendor/box64 HEAD=$$cur, expected $(BOX64_REF) (upstream v0.4.2)" >&2; \
		echo "check-vendor: run 'make vendor' or check out $(BOX64_REF) in vendor/box64" >&2; \
		exit 1; \
	fi

# Post-patch audit: submodule commit, pristine-upstream apply and worktree
# drift, all in tools/verify_box64_patch.sh. Run after `make patch` (or
# `make graft`). Nothing is hashed or locked: the verifier re-applies the queue
# to a pristine export and compares the files directly.
check-vendor-patches:
	tools/verify_box64_patch.sh --check

# Apply patches/ to vendor/box64 (idempotent). First build step. -N tolerates a
# queue that is already applied; --no-backup-if-mismatch keeps patch(1)'s
# pre-images (.orig) out of the submodule when a hunk lands at an offset.
patch: check-vendor $(PATCH_STAMP)

$(PATCH_STAMP): $(PATCHES)
	@set -e; \
	if [ -z "$(PATCHES)" ]; then \
		touch "$(PATCH_STAMP)"; \
		exit 0; \
	fi; \
	$(MAKE) --no-print-directory unpatch >/dev/null; \
	cd "$(BOX64)"; \
	for p in $(PATCHES); do \
		echo "patch: $$(basename "$$p")"; \
		if patch -p1 -N --no-backup-if-mismatch --dry-run -i "$$p" >/dev/null 2>&1; then \
			patch -p1 -N --no-backup-if-mismatch -i "$$p"; \
		elif patch -p1 -R --no-backup-if-mismatch --dry-run -i "$$p" >/dev/null 2>&1; then \
			echo "  (already applied)"; \
		else \
			echo "error: cannot apply $$p" >&2; \
			exit 1; \
		fi; \
	done; \
	cd "$(ROOT)" && touch "$(PATCH_STAMP)"

# Reverse patches/ (newest first). Used by distclean.
unpatch:
	@set -e; \
	if [ -z "$(PATCHES)" ]; then \
		rm -f "$(PATCH_STAMP)"; \
		exit 0; \
	fi; \
	cd "$(BOX64)"; \
	for p in $$(printf '%s\n' $(PATCHES) | sort -r); do \
		echo "unpatch: $$(basename "$$p")"; \
		if patch -p1 -R --no-backup-if-mismatch --dry-run -i "$$p" >/dev/null 2>&1; then \
			patch -p1 -R --no-backup-if-mismatch -i "$$p"; \
		elif patch -p1 -N --no-backup-if-mismatch --dry-run -i "$$p" >/dev/null 2>&1; then \
			echo "  (not applied)"; \
		else \
			echo "  (not applied: base absent)"; \
		fi; \
	done; \
	rm -f "$(PATCH_STAMP)"

# Drop graft symlinks left behind by a plugin that is no longer in
# $(PLUGIN_DIRS) — e.g. after a plugin directory was deleted from disk. graft
# installs each plugin's wrapped*.c / wrapped*_private.h as a symlink into
# $(BOX64)/src/wrapped, so an obsolete link is either dangling or points at a
# plugin directory this build no longer grafts.
#
# Guard rails, so this never becomes a blanket symlink cleanup:
#   * only symlinks are considered (regular files are skipped, including
#     upstream's tracked generated/wrappedlibnode*.h, which never match the
#     wrapped*.c / wrapped*_private.h names anyway);
#   * the resolved link target must live under $(ROOT)/plugins/;
#   * the basename must not be supplied by the current plugin set.
# Anything else — a real file, or a symlink resolving elsewhere — is left alone.
prune-stale-grafts:
	@set -e; \
	cur=""; \
	for d in $(PLUGIN_DIRS); do \
		for f in "$$d"/wrapped*.c "$$d"/wrapped*_private.h; do \
			[ -e "$$f" ] || continue; \
			cur="$$cur $$(basename "$$f")"; \
		done; \
	done; \
	for l in "$(BOX64)"/src/wrapped/wrapped*.c \
	         "$(BOX64)"/src/wrapped/wrapped*_private.h; do \
		[ -L "$$l" ] || continue; \
		tgt=$$(readlink -m "$$l") || continue; \
		case "$$tgt" in \
			"$(ROOT)"/plugins/*) ;; \
			*) continue ;; \
		esac; \
		case " $$cur " in \
			*" $$(basename "$$l") "*) continue ;; \
		esac; \
		echo "prune-stale-grafts: removing stale graft symlink $$l -> $$tgt"; \
		unlink "$$l"; \
	done

# Graft our sources into the vendored submodule. Everything here is a symlink or
# a generated file, so vendor/box64 stays re-checkoutable; the only hand-edits
# are the hook lines patches/ delivers (library_list.h, CMakeLists.txt).
# Patches under patches/ are applied first via the `patch` target.
#
# Fixed-arity call entries are implemented directly in src/libbox64.c as thin
# adapters to box64_call_simple(); there is no generated arity source to graft.
graft: patch prune-stale-grafts
	@set -e; \
	tmp_dir=$$(mktemp -d "$(BOX64)/.libbox64dl-graft.XXXXXX"); \
	trap 'rm -rf "$$tmp_dir"' 0 HUP INT TERM; \
	core_new="$$tmp_dir/box64dl_core.cmake.new"; \
	list_new="$$tmp_dir/library_list_extra.h.new"; \
	plugins_new="$$tmp_dir/box64dl_plugins.cmake.new"; \
	for f in $(VENDOR_SRC); do \
		ln -sfn "$$f" "$(BOX64)/src/$$(basename $$f)"; \
	done; \
	ln -sfn "$(ROOT)/src/core_shim.h" "$(BOX64)/src/core_shim.h"; \
	ln -sfn "$(ROOT)/src/native_module_lib.h" "$(BOX64)/src/native_module_lib.h"; \
	ln -sfn "$(NATIVE_SIG_SRC)" "$(BOX64)/src/$$(basename $(NATIVE_SIG_SRC))"; \
	ln -sfn "$(NATIVE_SIG_PRIVATE)" "$(BOX64)/src/$$(basename $(NATIVE_SIG_PRIVATE))"; \
	for f in $(ROOT)/include/*.h; do \
		ln -sfn "$$f" "$(BOX64)/src/include/$$(basename $$f)"; \
	done; \
	echo '# Generated by libbox64dl. Do not edit.' > "$$core_new"; \
	echo 'list(APPEND ELFLOADER_SRC' >> "$$core_new"; \
	for f in $(VENDOR_SRC); do \
		echo "    \"\$${BOX64_ROOT}/src/$$(basename $$f)\"" >> "$$core_new"; \
	done; \
	echo ')' >> "$$core_new"; \
	cmp -s "$$core_new" "$(BOX64)/box64dl_core.cmake" \
		|| mv "$$core_new" "$(BOX64)/box64dl_core.cmake"; \
	echo '/* Generated by libbox64dl. Do not edit. */' > "$$list_new"; \
	echo '# Generated by libbox64dl. Do not edit.' > "$$plugins_new"; \
	echo 'list(APPEND WRAPPEDS' >> "$$plugins_new"; \
	echo '    # Engine-owned native-module signature module: box64'"'"'s own wrapper' >> "$$plugins_new"; \
	echo '    # generator parses its _private.h, so the seam'"'"'s wrapper set comes from' >> "$$plugins_new"; \
	echo '    # the same generator as every wrapped library.' >> "$$plugins_new"; \
	echo "    \"\$${BOX64_ROOT}/src/$$(basename $(NATIVE_SIG_SRC))\"" >> "$$plugins_new"; \
	for d in $(PLUGIN_DIRS); do \
		test -f "$$d/library_list.inc" || { echo "plugin $$d: missing library_list.inc"; exit 1; }; \
		cat "$$d/library_list.inc" >> "$$list_new"; \
		for f in "$$d"/wrapped*.c "$$d"/wrapped*_private.h; do \
			ln -sfn "$$f" "$(BOX64)/src/wrapped/$$(basename $$f)"; \
		done; \
		for f in "$$d"/wrapped*.c; do \
			echo "    \"\$${BOX64_ROOT}/src/wrapped/$$(basename $$f)\"" \
				>> "$$plugins_new"; \
		done; \
	done; \
	echo ')' >> "$$plugins_new"; \
	cmp -s "$$list_new" "$(BOX64)/src/library_list_extra.h" \
		|| mv "$$list_new" "$(BOX64)/src/library_list_extra.h"; \
	cmp -s "$$plugins_new" "$(BOX64)/box64dl_plugins.cmake" \
		|| mv "$$plugins_new" "$(BOX64)/box64dl_plugins.cmake"; \
	echo "grafted plugins:$(notdir $(PLUGIN_DIRS)) + native signature module"

plugins:
	@set -e; i=0; \
	for d in $(PLUGIN_DIRS); do \
		i=$$((i+1)); \
		name=$$(printf '%s\n' $(PLUGIN_NAMES) | sed -n "$${i}p"); \
		[ -n "$$name" ] || name=$$(basename "$$d"); \
		echo "  $$name -> $$(readlink -f "$$d")"; \
	done

# Emit the resolved plugin set as a plugins.conf. Saving this output pins the
# build to explicit paths instead of whatever happens to be symlinked in.
plugin-manifest:
	@set -e; \
	i=0; \
	for d in $(PLUGIN_DIRS); do \
		i=$$((i+1)); \
		name=$$(printf '%s\n' $(PLUGIN_NAMES) | sed -n "$${i}p"); \
		[ -n "$$name" ] || name=$$(basename "$$d"); \
		printf '%s = %s\n' "$$name" "$$(readlink -f "$$d")"; \
	done

# Validate the resolved plugin set and surface cross-repository coupling: a
# plugin whose real path is outside $(ROOT) cannot be reproduced from this repo
# alone and should be pinned through plugins.conf in a release build.
check-plugins:
	@set -e; i=0; \
	for d in $(PLUGIN_DIRS); do \
		i=$$((i+1)); \
		name=$$(printf '%s\n' $(PLUGIN_NAMES) | sed -n "$${i}p"); \
		[ -n "$$name" ] || name=$$(basename "$$d"); \
		[ -d "$$d" ] || { echo "check-plugins: $$name ($$d) is not a directory"; exit 1; }; \
		for pat in library_list.inc 'wrapped*.c' 'wrapped*_private.h'; do \
			set -- "$$d"/$$pat; \
			[ -e "$$1" ] || { echo "check-plugins: $$name has no $$pat"; exit 1; }; \
		done; \
		real=$$(readlink -f "$$d"); \
		case "$$real/" in \
			"$(ROOT)"/*) echo "check-plugins: ok   $$name -> $$real" ;; \
			*) echo "check-plugins: WARN $$name -> $$real (outside $(ROOT))" ;; \
		esac; \
	done

$(BUILD_STATIC)/libbox64.a: patch graft
	@mkdir -p $(BUILD_STATIC)
	cd $(BUILD_STATIC) && cmake .. $(CMAKE_COMMON)
	$(MAKE) -C $(BUILD_STATIC) libbox64 -j$(NPROC)

$(BUILD_PIC)/libbox64.a: patch graft
	@mkdir -p $(BUILD_PIC)
	cd $(BUILD_PIC) && cmake .. $(CMAKE_PIC)
	$(MAKE) -C $(BUILD_PIC) libbox64 -j$(NPROC)

clean:
	rm -rf $(BUILD_STATIC) $(BUILD_PIC)

distclean: clean unpatch prune-stale-grafts
	@for f in $(VENDOR_SRC); do rm -f "$(BOX64)/src/$$(basename $$f)"; done
	rm -f "$(BOX64)/src/core_shim.h" "$(BOX64)/src/native_module_lib.h"
	rm -f "$(BOX64)/src/$$(basename $(NATIVE_SIG_SRC))" \
		"$(BOX64)/src/$$(basename $(NATIVE_SIG_PRIVATE))"
	@for f in $(ROOT)/include/*.h; do rm -f "$(BOX64)/src/include/$$(basename $$f)"; done
	rm -f $(BOX64)/src/library_list_extra.h \
		$(BOX64)/box64dl_plugins.cmake $(BOX64)/box64dl_core.cmake
	# Legacy ABI-1 graft output (the seam now builds the runtime wrapped
	# library in src/native_module_lib.c); remove it from old trees.
	rm -f $(BOX64)/src/box64dl_native_module_providers.h
	@for d in $(PLUGIN_DIRS); do \
		for f in "$$d"/wrapped*.c "$$d"/wrapped*_private.h; do \
			rm -f "$(BOX64)/src/wrapped/$$(basename $$f)"; \
		done; \
	done
