# libbox64dl — box64 as an embeddable library for loading x86_64 shared objects.
#
# Two flavours are built from one vendored tree:
#   build/      static, dynarec allowed — for host executables (unilib tests)
#   build-pic/  -fPIC, interpreter only — for .so/.node consumers (uninode)

ROOT := $(abspath .)
BOX64 := $(ROOT)/third_party/box64
BUILD_STATIC := $(BOX64)/build
BUILD_PIC := $(BOX64)/build-pic
NPROC := $(shell nproc)

# Ordered patches against the vendored tree (see patches/).
PATCHES := $(sort $(wildcard $(ROOT)/patches/*.patch))
PATCH_STAMP := $(BOX64)/.libbox64dl_patches_stamp

# Each plugins/<name>/ supplies wrapped<lib>.c, wrapped<lib>_private.h and
# library_list.inc. Downstream projects register by symlinking their directory
# in here (see uninode/Makefile).
PLUGIN_DIRS := $(wildcard $(ROOT)/plugins/*)

CMAKE_COMMON := -DLARCH64=1 -DCMAKE_BUILD_TYPE=RelWithDebInfo \
	-DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
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
	-DCMAKE_C_FLAGS="-fPIC -ftls-model=global-dynamic" \
	-DCMAKE_ASM_FLAGS="-fPIC"

.PHONY: all static pic patch unpatch graft plugins test test-detach-guest test-reset-stack clean distclean rust rust-static \
	rust-pic cli

all: static pic

# The thunk pool is self-contained, so it tests without building box64.
test: build/test_thunk
	./build/test_thunk

# White-box check of the host-side detach repair (libbox64_ext.h). It needs
# the full engine — it reads the real x64emu_t fields — but no wine: it fakes
# the post-attach RSP/GS shape and asserts the switch, its rollback and its
# fail-closed path. Links exactly like a host embedder.
TEST_DETACH_GUEST := $(ROOT)/build/test_detach_guest

$(TEST_DETACH_GUEST): tests/test_detach_guest.c $(BUILD_STATIC)/libbox64.a $(RUST_STATIC)
	@mkdir -p $(dir $@)
	$(CC) -Og -g -std=c11 -Wall -Wextra -D_GNU_SOURCE \
		-Iinclude -I$(BOX64)/src/include -I$(BOX64)/src -o $@ tests/test_detach_guest.c \
		-Wl,--whole-archive $(BUILD_STATIC)/libbox64.a $(BUILD_STATIC)/libdynarec.a \
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
		-Wl,--whole-archive $(BUILD_STATIC)/libbox64.a $(BUILD_STATIC)/libdynarec.a \
		-Wl,--no-whole-archive $(RUST_STATIC) \
		-lpthread -lm -ldl -lrt -lresolv -lutil -rdynamic

test-reset-stack: $(TEST_RESET_STACK)
	$(TEST_RESET_STACK)

# Rust facade. build.rs decides the flavor itself (feature/env), so these
# targets only ensure the matching box64 archive is up to date first. Each
# flavor keeps its own cargo target dir so both artifacts can coexist.
rust: rust-static
rust-static: $(BUILD_STATIC)/libbox64.a
	cd rust && CARGO_TARGET_DIR=target/static cargo build
rust-pic: $(BUILD_PIC)/libbox64.a
	cd rust && CARGO_TARGET_DIR=target/pic LIBBOX64_FLAVOR=pic cargo build

# ---- packaged box64 CLI: the runtime/bin/box64 an app tree ships ----
# Thin main (cli/box64_main.c) over the static engine, linked with the same
# shape embedders use: whole-archive the engine + dynarec, then the Rust
# facade that resolves the facade symbols libbox64.c references. Wine's
# companion children (wineserver/wineboot) re-exec through this binary
# exactly like through a system box64.
CLI := $(ROOT)/build/bin/box64
RUST_STATIC := $(ROOT)/rust/target/static/debug/liblibbox64.a

SHARED := $(ROOT)/build/lib/libbox64dl.so
RUST_PIC := $(ROOT)/rust/target/pic/debug/liblibbox64.a

cli: $(CLI)

shared: $(SHARED)

$(RUST_PIC): FORCE
	$(MAKE) rust-pic

$(SHARED): cli/entry.c $(BUILD_PIC)/libbox64.a $(RUST_PIC)
	@mkdir -p $(dir $@)
	$(CC) -fPIC -Iinclude -I$(BOX64)/src/include -shared -Wl,-soname,libbox64dl.so -Wl,-z,defs -o $@ cli/entry.c \
		-Wl,--whole-archive $(BUILD_PIC)/libbox64.a \
		$(BUILD_PIC)/libdynarec.a -Wl,--no-whole-archive \
		$(RUST_PIC) -lpthread -lm -ldl -lrt -lresolv -lutil

$(RUST_STATIC): FORCE
	$(MAKE) rust-static

FORCE:

$(CLI): cli/box64_main.c $(SHARED)
	@mkdir -p $(dir $@)
	$(CC) -O2 -std=c11 -D_GNU_SOURCE -I$(BOX64)/src/include -o $@ \
		cli/box64_main.c \
		-L$(dir $(SHARED)) -lbox64dl -Wl,-rpath,'$$ORIGIN/../lib'

build/test_thunk: tests/test_thunk.c tests/test_thunk_shim.S src/thunk.c src/thunk_la64.S
	@mkdir -p build
	$(CC) -Og -g -std=c11 -Wall -Wextra -Iinclude -o $@ $^ -lpthread

static: $(BUILD_STATIC)/libbox64.a
pic: $(BUILD_PIC)/libbox64.a

# Apply patches/ to third_party/box64 (idempotent). First build step.
patch: $(PATCH_STAMP)

$(PATCH_STAMP): $(PATCHES)
	@set -e; \
	if [ -z "$(PATCHES)" ]; then \
		touch "$(PATCH_STAMP)"; \
		exit 0; \
	fi; \
	cd "$(BOX64)"; \
	for p in $(PATCHES); do \
		echo "patch: $$(basename "$$p")"; \
		if patch -p1 -N --dry-run -i "$$p" >/dev/null 2>&1; then \
			patch -p1 -N -i "$$p"; \
		elif patch -p1 -R --dry-run -i "$$p" >/dev/null 2>&1; then \
			echo "  (already applied)"; \
		else \
			echo "error: cannot apply $$p" >&2; \
			exit 1; \
		fi; \
	done; \
	cd "$(ROOT)" && sha256sum $(PATCHES) > "$(PATCH_STAMP)"

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
		if patch -p1 -R --dry-run -i "$$p" >/dev/null 2>&1; then \
			patch -p1 -R -i "$$p"; \
		elif patch -p1 -N --dry-run -i "$$p" >/dev/null 2>&1; then \
			echo "  (not applied)"; \
		else \
			echo "error: cannot reverse $$p" >&2; \
			exit 1; \
		fi; \
	done; \
	rm -f "$(PATCH_STAMP)"

# Graft our sources into the vendored tree. Everything here is a symlink or a
# generated file, so third_party/box64 stays re-syncable with upstream; the only
# hand-edits there are the two hook lines in library_list.h and CMakeLists.txt.
# Patches under patches/ are applied first via the `patch` target.
graft: patch
	@for f in $(ROOT)/src/*.c $(ROOT)/src/*.S; do \
		ln -sfn "$$f" "$(BOX64)/src/$$(basename $$f)"; \
	done
	@for f in $(ROOT)/include/*.h; do \
		ln -sfn "$$f" "$(BOX64)/src/include/$$(basename $$f)"; \
	done
	@echo '# Generated by libbox64dl. Do not edit.' > $(BOX64)/box64dl_core.cmake.new
	@echo 'list(APPEND ELFLOADER_SRC' >> $(BOX64)/box64dl_core.cmake.new
	@for f in $(ROOT)/src/*.c $(ROOT)/src/*.S; do \
		echo "    \"\$${BOX64_ROOT}/src/$$(basename $$f)\"" >> $(BOX64)/box64dl_core.cmake.new; \
	done
	@echo ')' >> $(BOX64)/box64dl_core.cmake.new
	@cmp -s $(BOX64)/box64dl_core.cmake.new $(BOX64)/box64dl_core.cmake \
		|| mv $(BOX64)/box64dl_core.cmake.new $(BOX64)/box64dl_core.cmake
	@rm -f $(BOX64)/box64dl_core.cmake.new
	@echo '/* Generated by libbox64dl. Do not edit. */' > $(BOX64)/src/library_list_extra.h.new
	@echo '# Generated by libbox64dl. Do not edit.' > $(BOX64)/box64dl_plugins.cmake.new
	@echo 'list(APPEND WRAPPEDS' >> $(BOX64)/box64dl_plugins.cmake.new
	@for d in $(PLUGIN_DIRS); do \
		test -f "$$d/library_list.inc" || { echo "plugin $$d: missing library_list.inc"; exit 1; }; \
		cat "$$d/library_list.inc" >> $(BOX64)/src/library_list_extra.h.new; \
		for f in "$$d"/wrapped*.c "$$d"/wrapped*_private.h; do \
			ln -sfn "$$f" "$(BOX64)/src/wrapped/$$(basename $$f)"; \
		done; \
		for f in "$$d"/wrapped*.c; do \
			echo "    \"\$${BOX64_ROOT}/src/wrapped/$$(basename $$f)\"" \
				>> $(BOX64)/box64dl_plugins.cmake.new; \
		done; \
	done
	@echo ')' >> $(BOX64)/box64dl_plugins.cmake.new
	@# Only touch the real files on change, so cmake does not reconfigure needlessly.
	@cmp -s $(BOX64)/src/library_list_extra.h.new $(BOX64)/src/library_list_extra.h \
		|| mv $(BOX64)/src/library_list_extra.h.new $(BOX64)/src/library_list_extra.h
	@cmp -s $(BOX64)/box64dl_plugins.cmake.new $(BOX64)/box64dl_plugins.cmake \
		|| mv $(BOX64)/box64dl_plugins.cmake.new $(BOX64)/box64dl_plugins.cmake
	@rm -f $(BOX64)/src/library_list_extra.h.new $(BOX64)/box64dl_plugins.cmake.new
	@echo "grafted plugins:$(notdir $(PLUGIN_DIRS))"

plugins:
	@for d in $(PLUGIN_DIRS); do echo "  $$(basename $$d) -> $$(readlink -f $$d)"; done

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

distclean: clean unpatch
	@for f in $(ROOT)/src/*.c $(ROOT)/src/*.S; do rm -f "$(BOX64)/src/$$(basename $$f)"; done
	@for f in $(ROOT)/include/*.h; do rm -f "$(BOX64)/src/include/$$(basename $$f)"; done
	rm -f $(BOX64)/src/library_list_extra.h \
		$(BOX64)/box64dl_plugins.cmake $(BOX64)/box64dl_core.cmake
	@for d in $(PLUGIN_DIRS); do \
		for f in "$$d"/wrapped*.c "$$d"/wrapped*_private.h; do \
			rm -f "$(BOX64)/src/wrapped/$$(basename $$f)"; \
		done; \
	done
