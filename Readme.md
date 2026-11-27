# Libbox64dl

`Libbox64dl` lets you load and call an x64 `.so` from inside the host process,
even when the host itself is not x86_64. It builds on box64 for dynamic
loading and emulation.

## use

Build (requires the `ya_c_inline` repository alongside this one):

```bash
cargo build
cargo build --release
```

The Rust and box64 builds follow the same Cargo profile.  
Artifacts:

```text
target/debug/liblibbox64dl.so
target/release/liblibbox64dl.so
```

C programs can link with `-llibbox64dl`. The only exported symbol for now is `box64dl_init`, which is still a placeholder.

Run tests:

```bash
cargo run-test
cargo run-test --release
```

Patches in `patch/` are applied in filename order to a temporary copy of the pinned box64 submodule. `third_party/box64` is not modified.
## Finished
- Library-mode initialization path
- int,float,double,pointer
- call from C
- the ability of generating C stub(not fully generator)
## WIP
- struct,union,array,enum
- function pointer(call)
- function pointer(callback)
- JIT support
## Goal
- `dlopen` / `dlsym` / `dlclose` style API for x64 shared libraries
