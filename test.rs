use std::{
    env, fs,
    io::Write as _,
    path::{Path, PathBuf},
    process::Command,
};

const GREEN: &str = "\x1b[32m";
const RED: &str = "\x1b[31m";
const RESET: &str = "\x1b[0m";

const PASS_ART: &str = r#"
######      ##      ######    ######
#    #     #  #     #         #
#    #    #    #    #         #
######    ######    ######    ######
#         #    #         #         #
#         #    #         #         #
#         #    #    ######    ######
"#;

const FAILED_ART: &str = r#"
######      ##      ######    #         ######    #####
#          #  #       ##      #         #         #    #
#         #    #      ##      #         #         #    #
#####     ######      ##      #         #####     #    #
#         #    #      ##      #         #         #    #
#         #    #      ##      #         #         #    #
#         #    #    ######    ######    ######    #####
"#;

fn profile_dir() -> PathBuf {
    env::current_exe()
        .unwrap()
        .parent()
        .unwrap()
        .to_path_buf()
}

fn build_lib(release: bool) {
    let cargo = env::var("CARGO").unwrap_or_else(|_| "cargo".into());
    let mut cmd = Command::new(cargo);
    cmd.arg("build");
    if release {
        cmd.arg("--release");
    }
    assert!(cmd.status().unwrap().success(), "cargo build 失败");
}

fn test_dirs(root: &Path) -> Vec<PathBuf> {
    let mut dirs = Vec::new();

    for entry in fs::read_dir(root.join("tests")).unwrap() {
        let path = entry.unwrap().path();
        if path.is_dir() {
            dirs.push(path);
        }
    }

    dirs.sort();
    dirs
}

fn main() {
    let root = PathBuf::from(env!("CARGO_MANIFEST_DIR"));
    let profile_dir = profile_dir();
    let release = profile_dir.file_name().unwrap().to_string_lossy() == "release";

    build_lib(release);

    let make = env::var("MAKE").unwrap_or_else(|_| "make".into());
    let lib_arg = format!("LIB_DIR={}", profile_dir.display());
    let dirs = test_dirs(&root);
    let total = dirs.len().max(1);

    let mut passed = 0;
    let mut failed = 0;

    for (index, dir) in dirs.iter().enumerate() {
        let name = dir.file_name().unwrap().to_string_lossy();

        let _ = Command::new(&make).arg("clean").current_dir(dir).output();

        print!("[{:>3}%] {} ...", index * 100 / total, name);
        std::io::stdout().flush().unwrap();

        let result = Command::new(&make)
            .arg(&lib_arg)
            .arg("test")
            .current_dir(dir)
            .output();

        match result {
            Ok(output) if output.status.success() => {
                print!("\r[{:>3}%] {} ", (index + 1) * 100 / total, name);
                println!("{GREEN}ok{RESET}");
                passed += 1;
            }
            Ok(output) => {
                print!("\r[{:>3}%] {} ", (index + 1) * 100 / total, name);
                println!("{RED}failed{RESET}");
                std::io::stderr().write_all(&output.stderr).unwrap();
                failed += 1;
            }
            Err(error) => {
                print!("\r[{:>3}%] {} ", (index + 1) * 100 / total, name);
                println!("{RED}failed{RESET}");
                eprintln!("{error}");
                failed += 1;
            }
        }

        let _ = Command::new(&make).arg("clean").current_dir(dir).output();
    }

    if failed == 0 {
        println!("{GREEN}{PASS_ART}{RESET}");
        println!("passed: {passed}");
    } else {
        println!("{RED}{FAILED_ART}{RESET}");
        println!("passed: {passed}, failed: {failed}");
        std::process::exit(1);
    }
}
