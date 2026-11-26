use std::env;
use std::fs;
use std::path::{Path, PathBuf};
use std::process::Command;

fn main() {
    println!("cargo:rerun-if-changed=build.rs");
    println!("cargo:rerun-if-changed=third_party/box64");

    let out_dir = PathBuf::from(env::var("OUT_DIR").expect("OUT_DIR 未设置"));
    let target_arch = env::var("CARGO_CFG_TARGET_ARCH").expect("CARGO_CFG_TARGET_ARCH 未设置");
    let platform_flag = match target_arch.as_str() {
        "loongarch64" => Some("LARCH64"),
        "aarch64" => Some("ARM64"),
        "riscv64" => Some("RV64"),
        "powerpc64" => Some("PPC64LE"),
        "x86_64" => None,
        other => panic!("暂不支持的架构: {other}"),
    };

    let mut cflags: Vec<String> = [
        "src",
        "third_party/box64/src",
        "third_party/box64/src/include",
        "third_party/box64/src/emu",
    ]
    .iter()
    .map(|d| format!("-I{}", Path::new(d).canonicalize().unwrap().display()))
    .collect();
    if let Some(flag) = platform_flag {
        cflags.push(format!("-D{flag}"));
    }

    build_box64(&out_dir, platform_flag);
    ya_c_inline_build::Builder::new()
        .always_inline(true)
        .clang_args(cflags)
        .setup();
}

fn build_box64(out_dir: &Path, platform_flag: Option<&str>) {
    let profile = env::var("PROFILE").unwrap_or_else(|_| "debug".into());
    let cmake_profile = if profile == "release" { "Release" } else { "Debug" };
    let head = Command::new("git")
        .args(["-C", "third_party/box64", "rev-parse", "HEAD"])
        .output()
        .expect("无法读取 box64 commit");
    assert!(head.status.success(), "git rev-parse 失败");
    let sig = format!("{cmake_profile}-{}", String::from_utf8(head.stdout).unwrap().trim());

    let stamp = out_dir.join("box64.stamp");
    if !(out_dir.join("build/libbox64core.a").exists()
        && fs::read_to_string(&stamp).ok().as_deref() == Some(sig.as_str()))
    {
        let mut cmake = cmake::Config::new("third_party/box64");
        cmake
            .define("NOBOX64", "ON")
            .define("CMAKE_BUILD_TYPE", cmake_profile)
            .define("CMAKE_POSITION_INDEPENDENT_CODE", "ON");
        if let Some(flag) = platform_flag {
            cmake.define(flag, "ON");
        } else {
            cmake.define("LD80BITS", "ON").define("NOALIGN", "ON");
        }
        cmake.build_target("box64core").build();
        fs::write(&stamp, &sig).unwrap();
    }

    println!("cargo:rustc-link-search=native={}/build", out_dir.display());
    println!("cargo:rustc-link-lib=static=box64core");
    for lib in ["c", "m", "dl", "pthread", "resolv", "util"] {
        println!("cargo:rustc-link-lib={lib}");
    }
}
