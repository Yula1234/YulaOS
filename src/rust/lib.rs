#![no_std]

extern crate alloc;

use core::alloc::{GlobalAlloc, Layout};

use core::panic::PanicInfo;

pub mod libc;
pub mod drivers;
pub mod fs;
pub mod hal;
pub mod mm;

struct YulaAllocator;

unsafe impl GlobalAlloc for YulaAllocator {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        crate::mm::heap::kmalloc_aligned(layout.size(), layout.align() as u32) as *mut u8
    }

    unsafe fn dealloc(&self, ptr: *mut u8, _layout: Layout) {
        crate::mm::heap::kfree(ptr as *mut core::ffi::c_void);
    }
}

#[global_allocator]
static ALLOCATOR: YulaAllocator = YulaAllocator;

#[panic_handler]
fn panic(_info: &PanicInfo) -> ! {
    loop { }
}
