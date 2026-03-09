#[repr(C)]
pub struct VfsOps {
    pub read: Option<
        extern "C" fn(node: *mut VfsNode, offset: u32, size: u32, buffer: *mut u8) -> i32,
    >,
    pub write: Option<
        extern "C" fn(node: *mut VfsNode, offset: u32, size: u32, buffer: *const u8) -> i32,
    >,
    pub open: Option<extern "C" fn(node: *mut VfsNode) -> i32>,
    pub close: Option<extern "C" fn(node: *mut VfsNode) -> i32>,
    pub ioctl: Option<extern "C" fn(node: *mut VfsNode, req: u32, arg: *mut u8) -> i32>,
}

impl VfsOps {
    pub const fn empty() -> Self {
        Self {
            read: None,
            write: None,
            open: None,
            close: None,
            ioctl: None,
        }
    }

    pub const fn with_read_write(
        read: Option<
            extern "C" fn(node: *mut VfsNode, offset: u32, size: u32, buffer: *mut u8) -> i32,
        >,
        write: Option<
            extern "C" fn(node: *mut VfsNode, offset: u32, size: u32, buffer: *const u8) -> i32,
        >,
    ) -> Self {
        Self {
            read,
            write,
            open: None,
            close: None,
            ioctl: None,
        }
    }
}

#[repr(C)]
pub struct VfsNode {
    pub name: [u8; 32],
    pub flags: u32,
    pub size: u32,
    pub inode_idx: u32,
    pub refs: u32,
    pub fs_driver: *const u8,
    pub ops: *mut VfsOps,
    pub private_data: *mut u8,
    pub private_retain: Option<extern "C" fn(private_data: *mut u8)>,
    pub private_release: Option<extern "C" fn(private_data: *mut u8)>,
}

impl VfsNode {
    pub const fn new(name: [u8; 32]) -> Self {
        Self {
            name,
            flags: 0,
            size: 0,
            inode_idx: 0,
            refs: 0,
            fs_driver: core::ptr::null(),
            ops: core::ptr::null_mut(),
            private_data: core::ptr::null_mut(),
            private_retain: None,
            private_release: None,
        }
    }
}

pub const fn vfs_name32(cstr: &[u8]) -> [u8; 32] {
    let mut out = [0u8; 32];

    let mut i = 0usize;
    while i < out.len() && i < cstr.len() {
        let b = cstr[i];

        if b == 0 {
            break;
        }

        out[i] = b;
        i += 1;
    }

    out
}
