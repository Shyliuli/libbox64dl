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

.PHONY: all static pic patch unpatch graft plugins test test-detach-guest test-reset-stack test-call-arity test-call-plan test-call-fini bench-call-arity test-signal-policy test-signal-route test-guest-signal test-signal-library bench-signal-policy clean distclean rust rust-static \
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

# Regression fixture for the fixed-arity call entries (include/libbox64.h):
# a real x86_64 guest .so called through box64_call0..6, through the old
# register-file APIs, and through a re-entrant host callback. The guest is
# built by the x86_64 cross toolchain and linked freestanding (-nostdlib) so it
# carries no guest libc dependency; box64 dlopens it like any other guest ELF.
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
		-Wl,--whole-archive $(BUILD_STATIC)/libbox64.a $(BUILD_STATIC)/libdynarec.a \
		-Wl,--no-whole-archive $(RUST_STATIC) \
		-lpthread -lm -ldl -lrt -lresolv -lutil -rdynamic

test-call-arity: $(TEST_CALL_ARITY)
	@echo "== call-arity: dynarec =="
	$(TEST_CALL_ARITY) $(GUEST_CALL_SO)
	@echo "== call-arity: interpreter (BOX64_DYNAREC=0) =="
	BOX64_DYNAREC=0 $(TEST_CALL_ARITY) $(GUEST_CALL_SO)

# Microbenchmark for the call path (old register-file entries vs the
# fixed-arity fast entries, diagnostics off vs on). Prints ns/call to stderr.
# Run with `>/dev/null` so the diagnostics-on phase does not flood the log.
TEST_BENCH_ARITY := $(ROOT)/build/bench_call_arity

$(TEST_BENCH_ARITY): tests/bench_call_arity.c $(GUEST_CALL_SO) $(BUILD_STATIC)/libbox64.a
	@mkdir -p $(dir $@)
	$(CC) -O2 -g -std=c11 -Wall -Wextra -D_GNU_SOURCE \
		-Iinclude -I$(BOX64)/src/include -I$(BOX64)/src -o $@ tests/bench_call_arity.c \
		-Wl,--whole-archive $(BUILD_STATIC)/libbox64.a $(BUILD_STATIC)/libdynarec.a \
		-Wl,--no-whole-archive $(RUST_STATIC) \
		-lpthread -lm -ldl -lrt -lresolv -lutil -rdynamic

bench-call-arity: $(TEST_BENCH_ARITY)
	$(TEST_BENCH_ARITY) $(GUEST_CALL_SO) 200000

# Compiled-layout seam (box64_call_plan): register probe + x86_64-compiler
# oracle for aggregate placement (including the two-eightbyte s3f case).
TEST_CALL_PLAN := $(ROOT)/build/test_call_plan

$(TEST_CALL_PLAN): tests/test_call_plan.c $(GUEST_CALL_SO) $(BUILD_STATIC)/libbox64.a
	@mkdir -p $(dir $@)
	$(CC) -Og -g -std=c11 -Wall -Wextra -D_GNU_SOURCE \
		-Iinclude -I$(BOX64)/src/include -I$(BOX64)/src -o $@ tests/test_call_plan.c \
		-Wl,--whole-archive $(BUILD_STATIC)/libbox64.a $(BUILD_STATIC)/libdynarec.a \
		-Wl,--no-whole-archive $(RUST_STATIC) \
		-lpthread -lm -ldl -lrt -lresolv -lutil -rdynamic

test-call-plan: $(TEST_CALL_PLAN)
	$(TEST_CALL_PLAN) $(GUEST_CALL_SO)

# Lifetime guard: calls after box64_lib_fini() are refused, calls before run.
TEST_CALL_FINI := $(ROOT)/build/test_call_fini

$(TEST_CALL_FINI): tests/test_call_fini.c $(GUEST_CALL_SO) $(BUILD_STATIC)/libbox64.a
	@mkdir -p $(dir $@)
	$(CC) -Og -g -std=c11 -Wall -Wextra -D_GNU_SOURCE \
		-Iinclude -I$(BOX64)/src/include -I$(BOX64)/src -o $@ tests/test_call_fini.c \
		-Wl,--whole-archive $(BUILD_STATIC)/libbox64.a $(BUILD_STATIC)/libdynarec.a \
		-Wl,--no-whole-archive $(RUST_STATIC) \
		-lpthread -lm -ldl -lrt -lresolv -lutil -rdynamic

test-call-fini: $(TEST_CALL_FINI)
	$(TEST_CALL_FINI) $(GUEST_CALL_SO)

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
		-Wl,--whole-archive $(BUILD_STATIC)/libbox64.a $(BUILD_STATIC)/libdynarec.a \
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

# Fault-routing regression (src/signal_route.c): which faults box64 claims and
# which reach the pre-box64 host action. Separate concern from the mask policy;
# driven by real signals (host raise, host fault inside a bridge callback). A
# plain GS=0 Linux guest's JIT fault is a documented known gap and has no fake
# fixture here.
TEST_SIGNAL_ROUTE := $(ROOT)/build/test_signal_route

$(TEST_SIGNAL_ROUTE): tests/test_signal_route.c $(GUEST_CALL_SO) $(BUILD_STATIC)/libbox64.a
	@mkdir -p $(dir $@)
	$(CC) -Og -g -std=c11 -Wall -Wextra -D_GNU_SOURCE \
		-Iinclude -I$(BOX64)/src/include -I$(BOX64)/src -o $@ tests/test_signal_route.c \
		-Wl,--whole-archive $(BUILD_STATIC)/libbox64.a $(BUILD_STATIC)/libdynarec.a \
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
		-Wl,--whole-archive $(BUILD_STATIC)/libbox64.a $(BUILD_STATIC)/libdynarec.a \
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
		-Wl,--whole-archive $(BUILD_STATIC)/libbox64.a $(BUILD_STATIC)/libdynarec.a \
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

# Per-call signal-policy microbenchmark (the "policy cases" of the bench suite;
# the fixed-arity call-path numbers live in bench_call_arity). Fresh process per
# policy because the global default is immutable after init; numbers on stderr,
# library traces on stdout.
TEST_BENCH_SIGNAL := $(ROOT)/build/bench_signal_policy

$(TEST_BENCH_SIGNAL): tests/bench_signal_policy.c $(GUEST_CALL_SO) $(BUILD_STATIC)/libbox64.a
	@mkdir -p $(dir $@)
	$(CC) -O2 -g -std=c11 -Wall -Wextra -D_GNU_SOURCE \
		-Iinclude -I$(BOX64)/src/include -I$(BOX64)/src -o $@ tests/bench_signal_policy.c \
		-Wl,--whole-archive $(BUILD_STATIC)/libbox64.a $(BUILD_STATIC)/libdynarec.a \
		-Wl,--no-whole-archive $(RUST_STATIC) \
		-lpthread -lm -ldl -lrt -lresolv -lutil -rdynamic

bench-signal-policy: $(TEST_BENCH_SIGNAL)
	@mkdir -p build/test-results
	@echo "== bench signal-policy: global default (FAST) =="
	$(TEST_BENCH_SIGNAL) $(GUEST_CALL_SO) 200000 >/dev/null
	@echo "== bench signal-policy: global FAST + per-library COMPAT override =="
	$(TEST_BENCH_SIGNAL) $(GUEST_CALL_SO) 200000 --library-policy compat \
		--json build/test-results/performance.json >/dev/null

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
	$(MAKE) --no-print-directory unpatch >/dev/null; \
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
			echo "  (not applied: base absent)"; \
		fi; \
	done; \
	rm -f "$(PATCH_STAMP)"

# Graft our sources into the vendored tree. Everything here is a symlink or a
# generated file, so third_party/box64 stays re-syncable with upstream; the only
# hand-edits there are the two hook lines in library_list.h and CMakeLists.txt.
# Patches under patches/ are applied first via the `patch` target.
#
# src/libbox64_call_arity.inc is generated from tools/gen_call_arity.py and
# #included by src/libbox64.c, so it must be a grafted symlink next to it.
PYTHON ?= python3
GEN_ARITY := $(ROOT)/src/libbox64_call_arity.inc

$(GEN_ARITY): $(ROOT)/tools/gen_call_arity.py
	@if command -v $(PYTHON) >/dev/null 2>&1; then \
		$(PYTHON) $(ROOT)/tools/gen_call_arity.py; \
	elif [ ! -f "$@" ]; then \
		echo "error: $@ missing and $(PYTHON) not available" >&2; \
		exit 1; \
	else \
		echo "note: $(PYTHON) not available; using checked-in $@"; \
	fi

graft: patch $(GEN_ARITY)
	@for f in $(ROOT)/src/*.c $(ROOT)/src/*.S $(ROOT)/src/*.inc; do \
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
	@for f in $(ROOT)/src/*.c $(ROOT)/src/*.S $(ROOT)/src/*.inc; do rm -f "$(BOX64)/src/$$(basename $$f)"; done
	@for f in $(ROOT)/include/*.h; do rm -f "$(BOX64)/src/include/$$(basename $$f)"; done
	rm -f $(BOX64)/src/library_list_extra.h \
		$(BOX64)/box64dl_plugins.cmake $(BOX64)/box64dl_core.cmake
	@for d in $(PLUGIN_DIRS); do \
		for f in "$$d"/wrapped*.c "$$d"/wrapped*_private.h; do \
			rm -f "$(BOX64)/src/wrapped/$$(basename $$f)"; \
		done; \
	done
