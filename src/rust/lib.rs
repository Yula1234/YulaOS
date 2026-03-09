#![no_std]

use core::panic::PanicInfo;

pub mod libc;
pub mod drivers;
pub mod fs;
pub mod hal;
pub mod mm;

#[panic_handler]
fn panic(_info: &PanicInfo) -> ! {
    loop {
    }
}
