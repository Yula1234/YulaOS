use core::ffi::c_void;

type SizeT = usize;

pub const KERNEL_HEAP_START: u32 = 0xC000_0000;
pub const KERNEL_HEAP_SIZE: u32 = 0x4000_0000;

pub const PAGE_SIZE: u32 = 4096;

extern "C" {
    pub fn vmm_init();

    pub fn vmm_alloc_pages(pages: SizeT) -> *mut c_void;
    pub fn vmm_free_pages(virt: *mut c_void, pages: SizeT);

    pub fn vmm_map_page(virt: u32, phys: u32, flags: u32) -> i32;

    pub fn vmm_get_used_pages() -> SizeT;
}
