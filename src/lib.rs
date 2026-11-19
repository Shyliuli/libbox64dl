use abi_plan::Plan;
use std::ffi::*;

#[unsafe(no_mangle)]
unsafe extern "C" fn box64dl_init() {
    print!("TODO: box64dl_init");
    todo!();
}

#[unsafe(no_mangle)]
unsafe extern "C" fn box64dl_dlopen(path: *const c_char, flags: c_int) -> *mut c_void {
    print!("TODO: box64dl_dlopen");
    let _ = (path, flags);
    todo!();
}

#[unsafe(no_mangle)]
unsafe extern "C" fn box64dl_sym(handle: *mut c_void, name: *const c_char) -> *mut c_void {
    print!("TODO: box64dl_sym");
    let _ = (handle, name);
    todo!();
}

#[unsafe(no_mangle)]
unsafe extern "C" fn box64dl_close(handle: *mut c_void) -> c_int {
    print!("TODO: box64dl_close");
    let _ = handle;
    todo!();
}

#[unsafe(no_mangle)]
unsafe extern "C" fn box64dl_call(handle: *mut c_void, plan: *mut Plan) -> c_int {
    print!("TODO: box64dl_call");
    let _ = (handle, plan);
    todo!();
}
