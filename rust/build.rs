use std::env;
use std::path::PathBuf;

fn main() {
    let manifest = PathBuf::from(env::var("CARGO_MANIFEST_DIR").unwrap());
    let box64 = manifest.parent().unwrap().join("third_party/box64");
    let build = box64.join("build");
    let inc = box64.join("src/include");

    println!("cargo:rerun-if-changed=build.rs");
    println!("cargo:rerun-if-changed=shim.c");

    let mut cc = cc::Build::new();
    cc.file("shim.c").include(&inc).define("LA64", None);
    if build.join("libdynarec.a").is_file() {
        cc.define("DYNAREC", None);
    }
    cc.compile("box64_shim");

    println!("cargo:rustc-link-search=native={}", build.display());
    println!("cargo:rustc-link-lib=static=box64");
    if build.join("libdynarec.a").is_file() {
        println!("cargo:rustc-link-lib=static=dynarec");
    }
    println!("cargo:rustc-link-lib=dylib=dl");
    println!("cargo:rustc-link-lib=dylib=pthread");
    println!("cargo:rustc-link-lib=dylib=m");
}
