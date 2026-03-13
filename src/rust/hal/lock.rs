#[repr(C)]
pub struct SpinlockNative {
    tail: u32,
}

impl SpinlockNative {
    pub const fn new() -> Self {
        Self {
            tail: 0,
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
        unsafe {
            spinlock_acquire_native(spinlock_ptr(&self.native));
        }
    }

    pub fn release(&self) {
        unsafe {
            spinlock_release_native(spinlock_ptr(&self.native));
        }
    }

    pub fn acquire_safe(&self) -> u32 {
        unsafe { spinlock_acquire_safe_native(spinlock_ptr(&self.native)) }
    }

    pub fn release_safe(&self, flags: u32) {
        unsafe {
            spinlock_release_safe_native(spinlock_ptr(&self.native), flags);
        }
    }

    pub fn try_acquire(&self) -> bool {
        unsafe { spinlock_try_acquire_native(spinlock_ptr(&self.native)) != 0 }
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

fn spinlock_ptr(lock: &SpinlockNative) -> *mut SpinlockNative {
    lock as *const SpinlockNative as *mut SpinlockNative
}

extern "C" {
    fn spinlock_acquire_native(lock: *mut SpinlockNative);
    fn spinlock_acquire_safe_native(lock: *mut SpinlockNative) -> u32;
    fn spinlock_release_native(lock: *mut SpinlockNative);
    fn spinlock_release_safe_native(lock: *mut SpinlockNative, flags: u32);
    fn spinlock_try_acquire_native(lock: *mut SpinlockNative) -> i32;
}
