#[repr(C, align(4))]
pub struct DriverDesc {
    pub name: *const u8,
    pub klass: u32,
    pub stage: u32,
    pub init: Option<extern "C" fn() -> i32>,
    pub shutdown: Option<extern "C" fn()>,
}

unsafe impl Sync for DriverDesc {}

#[macro_export]
macro_rules! DRIVER_REGISTER {
    (
        $sym:ident,
        name: $name:expr,
        klass: $klass:expr,
        stage: $stage:expr,
        init: $init:expr
        $(, shutdown: $shutdown:expr)?
        $(,)?
    ) => {
        #[link_section = ".yosdrivers"]
        #[used]
        #[export_name = concat!("__yos_driver_", stringify!($sym))]
        pub static $sym: $crate::drivers::driver::DriverDesc = $crate::drivers::driver::DriverDesc {
            name: $name,
            klass: $klass,
            stage: $stage,
            init: Some($init),
            shutdown: $crate::DRIVER_REGISTER!(@shutdown $( $shutdown )?),
        };
    };

    (@shutdown) => {
        None
    };

    (@shutdown $shutdown:expr) => {
        Some($shutdown)
    };
}

pub const DRIVER_STAGE_EARLY: u32 = 0;
pub const DRIVER_STAGE_CORE: u32 = 1;
pub const DRIVER_STAGE_VFS: u32 = 2;
pub const DRIVER_STAGE_LATE: u32 = 3;

pub const DRIVER_CLASS_PSEUDO: u32 = 0;
pub const DRIVER_CLASS_CHAR: u32 = 1;
pub const DRIVER_CLASS_BLOCK: u32 = 2;
pub const DRIVER_CLASS_NET: u32 = 3;
pub const DRIVER_CLASS_GPU: u32 = 4;
pub const DRIVER_CLASS_INPUT: u32 = 5;
