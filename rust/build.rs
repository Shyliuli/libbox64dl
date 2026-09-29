use std::env;
use std::path::{Path, PathBuf};
use std::process::Command;

fn main() {
    let manifest = PathBuf::from(env::var("CARGO_MANIFEST_DIR").unwrap());
    let root = manifest.parent().unwrap().to_path_buf();
    let box64 = root.join("vendor/box64");

    // Flavor: executables link the non-PIC build/, .so consumers need the PIC
    // build-pic/. Selected by feature or by LIBBOX64_FLAVOR (make rust-pic).
    let pic = env::var("CARGO_FEATURE_PIC").is_ok()
        || env::var("LIBBOX64_FLAVOR")
            .map(|v| v == "pic")
            .unwrap_or(false);
    let flavor = if pic { "pic" } else { "static" };
    let build_dir = if let Ok(dir) = env::var("LIBBOX64_BUILD_DIR") {
        PathBuf::from(dir)
    } else if pic {
        box64.join("build-pic")
    } else {
        box64.join("build")
    };
    let dynarec = build_dir.join("libdynarec.a");

    println!("cargo:rerun-if-changed=build.rs");
    println!("cargo:rerun-if-changed=wrapper.h");
    println!(
        "cargo:rerun-if-changed={}",
        root.join("src/core_shim.c").display()
    );
    println!(
        "cargo:rerun-if-changed={}",
        root.join("src/core_shim.h").display()
    );
    println!("cargo:rerun-if-changed={}", root.join("Makefile").display());
    println!("cargo:rerun-if-env-changed=LIBBOX64_FLAVOR");

    let link_box64 = env::var("CARGO_FEATURE_LINK_BOX64").is_ok();
    if link_box64 {
        // Make is idempotent and owns patching/grafting/CMake, so build.rs does
        // not reimplement any of it: just ask for the selected flavor.
        let target_arch = env::var("CARGO_CFG_TARGET_ARCH").unwrap_or_default();
        let mut make = Command::new("make");
        make.arg("-C").arg(&root).arg(flavor);
        if target_arch == "x86_64" {
            make.env("LIBBOX64DL_ARCH", "x86_64");
            make.env("CC", "x86_64-linux-gnu-gcc");
            make.env("CXX", "x86_64-linux-gnu-g++");
        }
        let status = make.status()
            .expect("failed to run make for the vendored engine");
        assert!(status.success(), "make {flavor} failed");
    }

    // ---- bindgen: allowlist only our own shim + pt_mutex ----------------
    // libclang discovery: prefer an explicit LIBCLANG_PATH, else fall back to
    // the well-known llvm install dirs on this machine.
    if env::var_os("LIBCLANG_PATH").is_none() {
        for p in ["/usr/lib/llvm-20/lib", "/usr/lib/llvm-22/lib", "/usr/lib"] {
            if Path::new(p).join("libclang.so").exists()
                || Path::new(p).join("libclang.so.1").exists()
            {
                // SAFETY: build scripts are single-threaded here and nothing
                // else reads this variable concurrently.
                unsafe { env::set_var("LIBCLANG_PATH", p) };
                break;
            }
        }
    }
    let bindings = bindgen::Builder::default()
        .header("wrapper.h")
        .allowlist_function("box64dl_core_.*")
        .clang_arg(format!("-I{}", root.join("src").display()))
        .generate()
        .expect("bindgen failed for the private C shim");
    let out = PathBuf::from(env::var("OUT_DIR").unwrap());
    bindings
        .write_to_file(out.join("bindings.rs"))
        .expect("cannot write bindings.rs");

    // ---- compile the C shim + mutex primitive ---------------------------
    // The shim needs box64's private headers; that is exactly why it is C and
    // why the Rust business code does not see them.
    let mut cc = cc::Build::new();
    cc.file(root.join("src/core_shim.c"))
        .file(root.join("src/pt_mutex/pt_mutex.c"))
        .include(root.join("src"))
        .include(root.join("src/pt_mutex"))
        .include(root.join("include"))
        .include(box64.join("src/include"))
        .include(box64.join("src"));
    if env::var("CARGO_CFG_TARGET_ARCH").as_deref() == Ok("loongarch64") {
        cc.define("LA64", None);
    } else {
        cc.define("X86_64", None);
    }
    if pic {
        cc.flag("-fPIC");
    }
    if dynarec.is_file() {
        cc.define("DYNAREC", None);
    }
    cc.warnings(false).compile("box64_shim");

    if !link_box64 {
        // `cargo check --no-default-features`: type-check only, nothing to link.
        return;
    }

    // The engine archives are NOT linked with `cargo:rustc-link-lib=static`:
    // for the `staticlib` crate type rustc bundles those native archives into
    // liblibbox64.a, which duplicated the whole engine (and defeated the
    // "one engine archive" ownership rule). `cargo:rustc-link-arg` is applied
    // only when rustc actually invokes a linker (bin/test/example), so the
    // test harness gets the archives while the staticlib stays Rust+shim only.
    // The shipped DSO and the Makefile test binaries aggregate
    // build*/libbox64.a exactly once themselves.
    println!("cargo:rustc-link-search=native={}", build_dir.display());
    println!("cargo:rustc-link-lib=dylib=dl");
    println!("cargo:rustc-link-lib=dylib=pthread");
    println!("cargo:rustc-link-lib=dylib=m");

    // If make rebuilds the archives, rerun this script so rustc relinks.
    println!(
        "cargo:rerun-if-changed={}",
        build_dir.join("libbox64.a").display()
    );
    if dynarec.is_file() {
        println!("cargo:rerun-if-changed={}", dynarec.display());
    }

    // cargo test links the whole crate (through the shim), and box64.a and
    // libdynarec.a reference each other; a single-pass archive order cannot
    // resolve that, so use --start-group. The staticlib does not link, so
    // production builds are unaffected.
    println!("cargo:rustc-link-arg=-Wl,--start-group");
    println!(
        "cargo:rustc-link-arg={}",
        build_dir.join("libbox64.a").display()
    );
    if dynarec.is_file() {
        println!("cargo:rustc-link-arg={}", dynarec.display());
    }
    println!("cargo:rustc-link-arg=-Wl,--end-group");
    // The engine archives reference libm/libpthread/... and the linker sees
    // these after the archives only if they are appended here; the
    // rustc-link-lib=dylib entries above are emitted before the link-args and
    // would be dropped under --as-needed.
    for lib in ["m", "pthread", "dl", "rt", "resolv", "util", "c"] {
        println!("cargo:rustc-link-arg=-l{lib}");
    }
}
