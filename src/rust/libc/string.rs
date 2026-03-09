use core::ffi::{c_char, c_int, c_void};

type SizeT = usize;

extern "C" {
    pub fn strlen(s: *const c_char) -> SizeT;

    pub fn strcmp(a: *const c_char, b: *const c_char) -> c_int;
    pub fn strncmp(a: *const c_char, b: *const c_char, n: SizeT) -> c_int;

    pub fn memset(dst: *mut c_void, v: c_int, n: SizeT) -> *mut c_void;
    pub fn memcpy(dst: *mut c_void, src: *const c_void, n: SizeT) -> *mut c_void;
    pub fn memmove(dst: *mut c_void, src: *const c_void, n: SizeT) -> *mut c_void;
    pub fn memcmp(a: *const c_void, b: *const c_void, n: SizeT) -> c_int;

    pub fn strlcpy(dst: *mut c_char, src: *const c_char, dstsz: SizeT) -> SizeT;
    pub fn strlcat(dst: *mut c_char, src: *const c_char, dstsz: SizeT) -> SizeT;
}
