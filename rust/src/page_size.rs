use crate::c_box64;

pub fn init_pagesize() {
    let pagesize: i64 = unsafe { libc::sysconf(libc::_SC_PAGESIZE) };
    unsafe {
        c_box64::box64_pagesize = if pagesize > 0 {
            pagesize as usize
        } else {
            4096
        };
    }
}
