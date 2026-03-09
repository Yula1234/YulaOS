use core::ptr;

use crate::drivers::driver::DriverDesc;
use crate::fs::vfs::{VfsNode, VfsOps};

#[repr(C)]
pub struct Device {
    pub driver: *const DriverDesc,
    pub name: *const u8,
    pub flags: u32,
    pub private_data: *mut u8,
}

#[repr(C)]
pub struct CDevice {
    pub dev: Device,
    pub ops: VfsOps,
    pub node_template: VfsNode,
}

impl CDevice {
    pub const fn new(
        name: *const u8,
        node_name: [u8; 32],
        ops: VfsOps,
    ) -> Self {
        Self {
            dev: Device::new(name),
            ops,
            node_template: VfsNode::new(node_name),
        }
    }
}

impl Device {
    pub const fn new(name: *const u8) -> Self {
        Self {
            driver: ptr::null(),
            name,
            flags: 0,
            private_data: ptr::null_mut(),
        }
    }
}

extern "C" {
    pub fn cdevice_register(dev: *mut CDevice) -> i32;
}
