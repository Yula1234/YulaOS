#ifndef LIB_CPP_LOCK_GUARD_H
#define LIB_CPP_LOCK_GUARD_H

#include <hal/lock.h>

#include <stdint.h>

namespace kernel {

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

}

#endif
