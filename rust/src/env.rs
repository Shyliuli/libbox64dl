use std::sync::Once;

use crate::c_box64;

//保证不会重复初始化
static ENV_ONCE: Once = Once::new();
pub fn init_env() {
    ENV_ONCE.call_once(|| {
        unsafe {
            c_box64::LoadEnvVariables();
            c_box64::InitializeEnvFiles();
        }
        //设置默认值，当环境变量未设置时，设置为1
        if std::env::var_os("BOX64_NOBANNER").is_none() {
            unsafe {
                libc::setenv(c"BOX64_NOBANNER".as_ptr(), c"1".as_ptr(), 0);
            }
        }
    });
}
