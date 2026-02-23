#ifndef LIB_CPP_LOCK_GUARD_H
#define LIB_CPP_LOCK_GUARD_H

#include <hal/lock.h>

#include <stdint.h>

namespace kernel {

class ScopedIrqDisable {
public:
    ScopedIrqDisable() {
        uint32_t flags;
        __asm__ volatile(
            "pushfl\n\t"
            "popl %0\n\t"
            "cli"
            : "=r"(flags)
            :
            : "memory"
        );

        flags_ = flags;
    }

    ScopedIrqDisable(const ScopedIrqDisable&) = delete;
    ScopedIrqDisable& operator=(const ScopedIrqDisable&) = delete;

    ScopedIrqDisable(ScopedIrqDisable&&) = delete;
    ScopedIrqDisable& operator=(ScopedIrqDisable&&) = delete;

    void restore() {
        if (!active_) {
            return;
        }

        active_ = false;

        if ((flags_ & irq_if_mask) != 0u) {
            __asm__ volatile("sti");
        }
    }

    ~ScopedIrqDisable() {
        restore();
    }

private:
    static constexpr uint32_t irq_if_mask = 0x200u;

    uint32_t flags_ = 0u;
    bool active_ = true;
};

class SpinLock {
public:
    SpinLock() {
        spinlock_init(&lock_);
    }

    SpinLock(const SpinLock&) = delete;
    SpinLock& operator=(const SpinLock&) = delete;

    SpinLock(SpinLock&&) = delete;
    SpinLock& operator=(SpinLock&&) = delete;

    uint32_t acquire_safe() {
        return spinlock_acquire_safe(&lock_);
    }

    void release_safe(uint32_t flags) {
        spinlock_release_safe(&lock_, flags);
    }

    bool try_acquire() {
        return spinlock_try_acquire(&lock_) != 0;
    }

    void acquire() {
        spinlock_acquire(&lock_);
    }

    void release() {
        spinlock_release(&lock_);
    }

    spinlock_t* native_handle() {
        return &lock_;
    }

    const spinlock_t* native_handle() const {
        return &lock_;
    }

private:
    spinlock_t lock_;
};

class SpinLockSafeGuard {
public:
    explicit SpinLockSafeGuard(SpinLock& lock)
        : lock_(&lock),
          flags_(lock_->acquire_safe()) {
    }

    SpinLockSafeGuard(const SpinLockSafeGuard&) = delete;
    SpinLockSafeGuard& operator=(const SpinLockSafeGuard&) = delete;

    SpinLockSafeGuard(SpinLockSafeGuard&& other) = delete;
    SpinLockSafeGuard& operator=(SpinLockSafeGuard&& other) = delete;

    ~SpinLockSafeGuard() {
        if (lock_) {
            lock_->release_safe(flags_);
        }
    }

private:
    SpinLock* lock_;
    uint32_t flags_;
};

class SpinLockNativeSafeGuard {
public:
    explicit SpinLockNativeSafeGuard(spinlock_t& lock)
        : lock_(&lock),
          flags_(spinlock_acquire_safe(lock_)) {
    }

    SpinLockNativeSafeGuard(const SpinLockNativeSafeGuard&) = delete;
    SpinLockNativeSafeGuard& operator=(const SpinLockNativeSafeGuard&) = delete;

    SpinLockNativeSafeGuard(SpinLockNativeSafeGuard&&) = delete;
    SpinLockNativeSafeGuard& operator=(SpinLockNativeSafeGuard&&) = delete;

    ~SpinLockNativeSafeGuard() {
        if (lock_) {
            spinlock_release_safe(lock_, flags_);
        }
    }

private:
    spinlock_t* lock_ = nullptr;
    uint32_t flags_ = 0u;
};

class SpinLockNativeGuard {
public:
    explicit SpinLockNativeGuard(spinlock_t& lock)
        : lock_(&lock) {
        spinlock_acquire(lock_);
    }

    SpinLockNativeGuard(const SpinLockNativeGuard&) = delete;
    SpinLockNativeGuard& operator=(const SpinLockNativeGuard&) = delete;

    SpinLockNativeGuard(SpinLockNativeGuard&&) = delete;
    SpinLockNativeGuard& operator=(SpinLockNativeGuard&&) = delete;

    ~SpinLockNativeGuard() {
        if (lock_) {
            spinlock_release(lock_);
        }
    }

private:
    spinlock_t* lock_ = nullptr;
};

class SpinLockGuard {
public:
    explicit SpinLockGuard(SpinLock& lock)
        : lock_(&lock) {
        lock_->acquire();
    }

    SpinLockGuard(const SpinLockGuard&) = delete;
    SpinLockGuard& operator=(const SpinLockGuard&) = delete;

    SpinLockGuard(SpinLockGuard&& other) = delete;
    SpinLockGuard& operator=(SpinLockGuard&& other) = delete;

    ~SpinLockGuard() {
        if (lock_) {
            lock_->release();
        }
    }

private:
    SpinLock* lock_;
};

class TrySpinLockGuard {
public:
    explicit TrySpinLockGuard(SpinLock& lock)
        : lock_(&lock),
          acquired_(lock.try_acquire()) {
    }

    TrySpinLockGuard(const TrySpinLockGuard&) = delete;
    TrySpinLockGuard& operator=(const TrySpinLockGuard&) = delete;

    TrySpinLockGuard(TrySpinLockGuard&&) = delete;
    TrySpinLockGuard& operator=(TrySpinLockGuard&&) = delete;

    ~TrySpinLockGuard() {
        if (acquired_) {
            lock_->release();
        }
    }

    explicit operator bool() const noexcept {
        return acquired_;
    }

private:
    SpinLock* lock_;
    bool acquired_;
};

}

#endif
