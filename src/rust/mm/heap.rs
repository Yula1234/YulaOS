use core::ffi::{c_char, c_int, c_void};

type SizeT = usize;

#[repr(C)]
pub struct KmemCache {
    _private: [u8; 0],
}

extern "C" {
    pub fn heap_init();

    pub fn kmalloc(size: SizeT) -> *mut c_void;
    pub fn kzalloc(size: SizeT) -> *mut c_void;
    pub fn krealloc(ptr: *mut c_void, new_size: SizeT) -> *mut c_void;
    pub fn kfree(ptr: *mut c_void);

    pub fn kmalloc_aligned(size: SizeT, align: u32) -> *mut c_void;
    pub fn kmalloc_a(size: SizeT) -> *mut c_void;

    pub fn kmem_cache_create(
        name: *const c_char,
        size: SizeT,
        align: u32,
        flags: u32,
    ) -> *mut KmemCache;

    pub fn kmem_cache_alloc(cache: *mut KmemCache) -> *mut c_void;
    pub fn kmem_cache_free(cache: *mut KmemCache, obj: *mut c_void);

    pub fn kmem_cache_destroy(cache: *mut KmemCache) -> c_int;
}
