use std::env;
use std::path::PathBuf;
use std::process::Command;

fn main() {
    let manifest = PathBuf::from(env::var("CARGO_MANIFEST_DIR").unwrap());
    let root = manifest.parent().unwrap().to_path_buf();
    let box64 = root.join("third_party/box64");

    // Flavor: executables link the non-PIC build/, .so/.node consumers need the
    // PIC build-pic/. Selected by feature or by LIBBOX64_FLAVOR (make rust-pic).
    let pic = env::var("CARGO_FEATURE_PIC").is_ok()
        || env::var("LIBBOX64_FLAVOR").map(|v| v == "pic").unwrap_or(false);
    let flavor = if pic { "pic" } else { "static" };
    let build_dir = if pic { box64.join("build-pic") } else { box64.join("build") };
    let dynarec = build_dir.join("libdynarec.a");

    println!("cargo:rerun-if-changed=build.rs");
    println!("cargo:rerun-if-changed=shim.c");
    println!("cargo:rerun-if-changed={}", root.join("Makefile").display());
    println!("cargo:rerun-if-env-changed=LIBBOX64_FLAVOR");

    let link_box64 = env::var("CARGO_FEATURE_LINK_BOX64").is_ok();
    if link_box64 {
        // Make is idempotent and owns patching/grafting/CMake, so build.rs does
        // not reimplement any of it: just ask for the selected flavor.
        let status = Command::new("make")
            .arg("-C")
            .arg(&root)
            .arg(flavor)
            .status()
            .expect("failed to run make for box64");
        assert!(status.success(), "make {flavor} failed");
    }

    // Match box64 CMakeLists include_directories: src/include + src
    // (src is required for dynarec/native_lock.h pulled by box64context.h).
    let mut cc = cc::Build::new();
    cc.file("shim.c")
        .include(box64.join("src/include"))
        .include(box64.join("src"))
        .define("LA64", None);
    if pic {
        cc.flag("-fPIC");
    }
    if dynarec.is_file() {
        cc.define("DYNAREC", None);
    }
    cc.compile("box64_shim");

    if !link_box64 {
        // `cargo check --no-default-features`: type-check only, nothing to link.
        return;
    }

    println!("cargo:rustc-link-search=native={}", build_dir.display());
    println!("cargo:rustc-link-lib=static=box64");
    if dynarec.is_file() {
        println!("cargo:rustc-link-lib=static=dynarec");
    }
    println!("cargo:rustc-link-lib=dylib=dl");
    println!("cargo:rustc-link-lib=dylib=pthread");
    println!("cargo:rustc-link-lib=dylib=m");

    // If make rebuilds the archives, rerun this script so rustc relinks.
    println!("cargo:rerun-if-changed={}", build_dir.join("libbox64.a").display());
    if dynarec.is_file() {
        println!("cargo:rerun-if-changed={}", dynarec.display());
    }
}
