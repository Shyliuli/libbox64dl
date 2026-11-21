use std::env;
use std::path::*;
use std::fs;
use std::collections::hash_map::*;
use std::hash::*;
use std::process::{Command, Stdio};


fn main() {
    let mut c_include_path = env::var("C_INCLUDE_PATH").unwrap_or_default();
    for dir in ["src", "tests", "third_party/box64/src", "third_party/box64/src/include", "third_party/box64/src/emu"] {
        if !c_include_path.is_empty() {
            c_include_path.push(':');
        }
        c_include_path.push_str(&Path::new(dir).canonicalize().unwrap().to_string_lossy());
    }
    unsafe { env::set_var("C_INCLUDE_PATH", c_include_path) };

    println!("cargo:rerun-if-changed=third_party/box64");
    println!("cargo:rerun-if-changed=patch");
    println!("cargo:rerun-if-changed=src/box64dl_cinclude.h");
    println!("cargo:rerun-if-changed=src/box64dl_bindgen.h");
    let out_dir = PathBuf::from(env::var("OUT_DIR").expect("OUT_DIR 未设置"));
    let target_arch = env::var("CARGO_CFG_TARGET_ARCH")
        .expect("CARGO_CFG_TARGET_ARCH 未设置");

    let platform_flag = match target_arch.as_str() {
        "loongarch64" => Some("LARCH64"),
        "aarch64" => Some("ARM64"),
        "riscv64" => Some("RV64"),
        "powerpc64" => Some("PPC64LE"),
        "x86_64" => None,
        other => panic!("暂不支持的架构: {other}"),
    };
    write_public_header();
    generate_bindings(&out_dir, platform_flag);
    ya_c_inline_build::Builder::new().always_inline(true).setup();

    let cargo_profile = env::var("PROFILE").unwrap_or_else(|_| "debug".into());
    let cmake_profile = if cargo_profile == "release" { "Release" } else { "Debug" };

    // 复制box64代码到target/.../box64-src，并应用patch
    let source_dir = prepare_source(&out_dir);

    let mut cmake = cmake::Config::new(source_dir);
    cmake
        .define("NOBOX64", "ON")
        .define("CMAKE_BUILD_TYPE", cmake_profile)
        .define("CMAKE_POSITION_INDEPENDENT_CODE", "ON");
    if let Some(flag) = platform_flag {
        cmake.define(flag, "ON");
    } else {
        // x86_64 上按 box64 官方 x86_64 Linux 配置走纯解释器
        cmake.define("LD80BITS", "ON").define("NOALIGN", "ON");
    }
    let dst = cmake.build_target("box64core").build();



    println!("cargo:rustc-link-search=native={}/build", dst.display());
    println!("cargo:rustc-link-lib=static=box64core");
    //box64 target_link_libraries(${BOX64} c m dl pthread resolv util c)
    for lib in ["c", "m", "dl", "pthread", "resolv", "util"] {
        println!("cargo:rustc-link-lib={lib}");
    }
}

fn prepare_source(out_dir: &Path) -> PathBuf {
    let source_dir = out_dir.join("box64-src");
    let stamp_path = out_dir.join("box64-source-stamp");

    let signature = source_signature();
    let old_signature = fs::read_to_string(&stamp_path).ok();
    //代码没有修改
    if old_signature.as_deref() == Some(signature.as_str()) && source_dir.exists() {
        return source_dir;
    }

    if source_dir.exists() {
        fs::remove_dir_all(&source_dir).unwrap();
    }
    fs::create_dir_all(&source_dir).unwrap();

    let mut git = Command::new("git")
        .args(["-C", "third_party/box64", "archive", "HEAD"])
        .stdout(Stdio::piped())
        .spawn()
        .expect("无法运行 git archive");

    let mut tar = Command::new("tar")
        .args(["-x", "-C"])
        .arg(&source_dir)
        .stdin(git.stdout.take().unwrap())
        .spawn()
        .expect("无法运行 tar");

    assert!(tar.wait().unwrap().success(), "tar 失败");
    assert!(git.wait().unwrap().success(), "git archive 失败");

    apply_patches(&source_dir);

    fs::write(&stamp_path, &signature).unwrap();

    source_dir
}


fn patch_files() -> Vec<PathBuf> {
    let mut patches = Vec::new();

    for entry in fs::read_dir("patch").unwrap() {
        let path = entry.unwrap().path();
        if path.is_file() {
            patches.push(fs::canonicalize(&path).unwrap());
        }
    }

    patches.sort();
    patches
}

fn source_signature() -> String {
    let output = Command::new("git")
        .args(["-C", "third_party/box64", "rev-parse", "HEAD"])
        .output()
        .expect("无法读取 box64 commit");

    assert!(output.status.success(), "git rev-parse 失败");

    let commit = String::from_utf8(output.stdout)
        .unwrap()
        .trim()
        .to_string();

    let mut hasher = DefaultHasher::new();
    commit.hash(&mut hasher);

    for patch in patch_files() {
        patch.hash(&mut hasher);
        fs::read(&patch).unwrap().hash(&mut hasher);
    }

    format!("{}-{:016x}", commit, hasher.finish())
}

fn apply_patches(source_dir: &Path) {
    for patch in patch_files() {
        let status = Command::new("patch")
            .args([
                "-p1",
                "--forward",
                "--batch",
                "--no-backup-if-mismatch",
                "--fuzz=0",
                "-d",
            ])
            .arg(source_dir)
            .arg("-i")
            .arg(&patch)
            .status()
            .expect("无法运行 patch");

        assert!(
            status.success(),
            "patch 应用失败: {}",
            patch.display()
        );
    }
}

fn write_public_header() {
    let crate_dir = env::var("CARGO_MANIFEST_DIR").unwrap();
    cbindgen::Builder::new()
        .with_crate(crate_dir)
        .with_language(cbindgen::Language::C)
        .with_include_guard("BOX64DL_H_")
        .with_parse_deps(true)
        .with_parse_include(&["abi-plan"])
        .generate()
        .expect("无法生成 src/box64.h")
        .write_to_file("src/box64.h");
}

fn generate_bindings(out_dir: &Path, platform_flag: Option<&str>) {
    let header = PathBuf::from("src/box64dl_bindgen.h");
    let mut builder = bindgen::Builder::default()
        .header(header.to_string_lossy())
        .clang_arg("-Ithird_party/box64/src/include")
        .rust_target(bindgen::RustTarget::stable(82, 0).unwrap())
        .opaque_type(".*")
        .layout_tests(false);
    if let Some(flag) = platform_flag {
        builder = builder.clang_arg(format!("-D{flag}"));
    }
    builder
        .generate()
        .unwrap()
        .write_to_file(out_dir.join("box64_bindings.rs"))
        .unwrap();
}
