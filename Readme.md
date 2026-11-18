# Libbox64dl

`Libbox64dl` lets you load and call an x64 `.so` from inside the host process,
even when the host itself is not x86_64. It builds on box64 for dynamic
loading and emulation.

## use

Build:

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

## WIP
- Library-mode initialization path

## Goal
- `dlopen` / `dlsym` / `dlclose` style API for x64 shared libraries
