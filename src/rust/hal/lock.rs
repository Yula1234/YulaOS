use core::arch::asm;

#[repr(C)]
pub struct SpinlockNative {
    locked: u32,
}

impl SpinlockNative {
    pub const fn new() -> Self {
        Self {
            locked: 0,
        }
    }
}

pub struct SpinLock {
    native: SpinlockNative,
}

impl SpinLock {
    pub const fn new() -> Self {
        Self {
            native: SpinlockNative::new(),
        }
    }

    pub fn acquire(&self) {
        spinlock_acquire(&self.native);
    }

    pub fn release(&self) {
        spinlock_release(&self.native);
    }

    pub fn acquire_safe(&self) -> u32 {
        spinlock_acquire_safe(&self.native)
    }

    pub fn release_safe(&self, flags: u32) {
        spinlock_release_safe(&self.native, flags);
    }

    pub fn try_acquire(&self) -> bool {
        spinlock_try_acquire(&self.native)
    }

    pub fn native_handle(&self) -> *const SpinlockNative {
        &self.native
    }

    pub fn native_handle_mut(&mut self) -> *mut SpinlockNative {
        &mut self.native
    }
}

pub struct SpinLockGuard<'a> {
    lock: &'a SpinLock,
}

impl<'a> SpinLockGuard<'a> {
    pub fn new(lock: &'a SpinLock) -> Self {
        lock.acquire();

        Self {
            lock,
        }
    }
}

impl Drop for SpinLockGuard<'_> {
    fn drop(&mut self) {
        self.lock.release();
    }
}

pub struct SpinLockSafeGuard<'a> {
    lock: &'a SpinLock,
    flags: u32,
}

impl<'a> SpinLockSafeGuard<'a> {
    pub fn new(lock: &'a SpinLock) -> Self {
        let flags = lock.acquire_safe();

        Self {
            lock,
            flags,
        }
    }
}

impl Drop for SpinLockSafeGuard<'_> {
    fn drop(&mut self) {
        self.lock.release_safe(self.flags);
    }
}

pub struct TrySpinLockGuard<'a> {
    lock: &'a SpinLock,
    acquired: bool,
}

impl<'a> TrySpinLockGuard<'a> {
    pub fn new(lock: &'a SpinLock) -> Self {
        let acquired = lock.try_acquire();

        Self {
            lock,
            acquired,
        }
    }

    pub fn acquired(&self) -> bool {
        self.acquired
    }
}

impl Drop for TrySpinLockGuard<'_> {
    fn drop(&mut self) {
        if self.acquired {
            self.lock.release();
        }
    }
}

impl core::ops::Deref for TrySpinLockGuard<'_> {
    type Target = bool;

    fn deref(&self) -> &Self::Target {
        &self.acquired
    }
}

fn cpu_pause() {
    unsafe {
        asm!("pause", options(nomem, nostack, preserves_flags));
    }
}

fn cpu_cli() {
    unsafe {
        asm!("cli", options(nomem, nostack, preserves_flags));
    }
}

fn cpu_sti() {
    unsafe {
        asm!("sti", options(nomem, nostack, preserves_flags));
    }
}

fn cpu_read_eflags() -> u32 {
    let flags: u32;

    unsafe {
        asm!(
            "pushfd",
            "pop {out}",
            out = out(reg) flags,
            options(nomem, preserves_flags)
        );
    }

    flags
}

unsafe fn atomic_xchg_u32(ptr: *mut u32, value: u32) -> u32 {
    let mut value = value;

    asm!(
        "xchg [{ptr}], {value}",
        ptr = in(reg) ptr,
        value = inout(reg) value,
        options(nostack, preserves_flags),
    );

    value
}

fn spinlock_locked_ptr(lock: &SpinlockNative) -> *mut u32 {
    core::ptr::addr_of!(lock.locked) as *mut u32
}

pub fn spinlock_acquire(lock: &SpinlockNative) {
    let mut backoff: u32 = 1;

    loop {
        while unsafe { core::ptr::read_volatile(&lock.locked) } != 0 {
            for _ in 0..backoff {
                cpu_pause();
            }

            if backoff < 1024 {
                backoff <<= 1;
            }
        }

        let locked_ptr = spinlock_locked_ptr(lock);
        if unsafe { atomic_xchg_u32(locked_ptr, 1) } == 0 {
            return;
        }

        cpu_pause();
    }
}

pub fn spinlock_try_acquire(lock: &SpinlockNative) -> bool {
    let locked_ptr = spinlock_locked_ptr(lock);

    unsafe { atomic_xchg_u32(locked_ptr, 1) == 0 }
}

pub fn spinlock_release(lock: &SpinlockNative) {
    let locked_ptr = spinlock_locked_ptr(lock);

    unsafe {
        atomic_xchg_u32(locked_ptr, 0);
    }
}

pub fn spinlock_acquire_safe(lock: &SpinlockNative) -> u32 {
    let flags = cpu_read_eflags();

    cpu_cli();

    spinlock_acquire(lock);

    flags
}

pub fn spinlock_release_safe(lock: &SpinlockNative, flags: u32) {
    spinlock_release(lock);

    if (flags & 0x200) != 0 {
        cpu_sti();
    }
}
