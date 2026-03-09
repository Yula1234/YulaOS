use core::ffi::c_void;

pub const PAGE_SIZE: u32 = 4096;
pub const PAGE_SHIFT: u32 = 12;

pub const PMM_MAX_ORDER: u32 = 11;

pub const PMM_FLAG_FREE: u32 = 0;
pub const PMM_FLAG_USED: u32 = 1u32 << 0;
pub const PMM_FLAG_KERNEL: u32 = 1u32 << 1;
pub const PMM_FLAG_DMA: u32 = 1u32 << 2;

pub const PMM_ZONE_DMA: u32 = 0;
pub const PMM_ZONE_NORMAL: u32 = 1;
pub const PMM_ZONE_COUNT: u32 = 2;

pub const PMM_REGION_AVAILABLE: u32 = 1;
pub const PMM_REGION_RESERVED: u32 = 2;

#[repr(C)]
pub struct PmmRegion {
    pub base: u32,
    pub size: u32,
    pub r#type: u32,
}

#[repr(C)]
pub struct PmmReservedRegion {
    pub base: u32,
    pub size: u32,
}

#[repr(C)]
pub struct Page {
    pub flags: u32,
    pub ref_count: i32,

    pub order_or_slab_cache: *mut c_void,

    pub freelist: *mut c_void,
    pub objects: u16,

    pub prev: *mut Page,
    pub next: *mut Page,

    pub _reserved: u32,
}

extern "C" {
    pub fn pmm_init(mem_size: u32, kernel_end_addr: u32);

    pub fn pmm_init_regions(
        regions: *const PmmRegion,
        region_count: u32,
        reserved: *const PmmReservedRegion,
        reserved_count: u32,
        kernel_end_addr: u32,
    );

    pub fn pmm_alloc_block() -> *mut c_void;
    pub fn pmm_free_block(addr: *mut c_void);

    pub fn pmm_alloc_pages(order: u32) -> *mut c_void;
    pub fn pmm_alloc_pages_zone(order: u32, zone: u32) -> *mut c_void;
    pub fn pmm_free_pages(addr: *mut c_void, order: u32);

    pub fn pmm_phys_to_page(phys_addr: u32) -> *mut Page;
    pub fn pmm_page_to_phys(page: *mut Page) -> u32;

    pub fn pmm_get_used_blocks() -> u32;
    pub fn pmm_get_free_blocks() -> u32;
    pub fn pmm_get_total_blocks() -> u32;
}
