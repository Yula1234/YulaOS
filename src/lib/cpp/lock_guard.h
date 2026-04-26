#ifndef LIB_CPP_LOCK_GUARD_H
#define LIB_CPP_LOCK_GUARD_H

#include <kernel/locking/rwspinlock.h>
#include <kernel/locking/spinlock.h>
#include <kernel/locking/rwlock.h>
#include <kernel/locking/mutex.h>

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

class MutexNativeGuard {
public:
    explicit MutexNativeGuard(mutex_t& lock)
        : lock_(&lock) {
        mutex_lock(lock_);
    }

    MutexNativeGuard(const MutexNativeGuard&) = delete;
    MutexNativeGuard& operator=(const MutexNativeGuard&) = delete;

    MutexNativeGuard(MutexNativeGuard&&) = delete;
    MutexNativeGuard& operator=(MutexNativeGuard&&) = delete;

    ~MutexNativeGuard() {
        if (lock_) {
            mutex_unlock(lock_);
        }
    }

private:
    mutex_t* lock_;
};

class RwLockNativeReadGuard {
public:
    explicit RwLockNativeReadGuard(rwlock_t& lock)
        : lock_(&lock) {
        rwlock_acquire_read(lock_);
    }

    RwLockNativeReadGuard(const RwLockNativeReadGuard&) = delete;
    RwLockNativeReadGuard& operator=(const RwLockNativeReadGuard&) = delete;

    RwLockNativeReadGuard(RwLockNativeReadGuard&&) = delete;
    RwLockNativeReadGuard& operator=(RwLockNativeReadGuard&&) = delete;

    ~RwLockNativeReadGuard() {
        if (lock_) {
            rwlock_release_read(lock_);
        }
    }

private:
    rwlock_t* lock_ = nullptr;
};

class RwLockNativeWriteGuard {
public:
    explicit RwLockNativeWriteGuard(rwlock_t& lock)
        : lock_(&lock) {
        rwlock_acquire_write(lock_);
    }

    RwLockNativeWriteGuard(const RwLockNativeWriteGuard&) = delete;
    RwLockNativeWriteGuard& operator=(const RwLockNativeWriteGuard&) = delete;

    RwLockNativeWriteGuard(RwLockNativeWriteGuard&&) = delete;
    RwLockNativeWriteGuard& operator=(RwLockNativeWriteGuard&&) = delete;

    ~RwLockNativeWriteGuard() {
        if (lock_) {
            rwlock_release_write(lock_);
        }
    }

private:
    rwlock_t* lock_ = nullptr;
};

class RwSpinLockNativeReadSafeGuard {
public:
    explicit RwSpinLockNativeReadSafeGuard(rwspinlock_t& lock)
        : lock_(&lock),
          flags_(rwspinlock_acquire_read_safe(lock_)) {
    }

    RwSpinLockNativeReadSafeGuard(const RwSpinLockNativeReadSafeGuard&) = delete;
    RwSpinLockNativeReadSafeGuard& operator=(const RwSpinLockNativeReadSafeGuard&) = delete;

    RwSpinLockNativeReadSafeGuard(RwSpinLockNativeReadSafeGuard&&) = delete;
    RwSpinLockNativeReadSafeGuard& operator=(RwSpinLockNativeReadSafeGuard&&) = delete;

    ~RwSpinLockNativeReadSafeGuard() {
        if (lock_) {
            rwspinlock_release_read_safe(lock_, flags_);
        }
    }

private:
    rwspinlock_t* lock_ = nullptr;
    uint32_t flags_ = 0u;
};

class RwSpinLockNativeReadGuard {
public:
    explicit RwSpinLockNativeReadGuard(rwspinlock_t& lock)
        : lock_(&lock) {
        rwspinlock_acquire_read(lock_);
    }

    RwSpinLockNativeReadGuard(const RwSpinLockNativeReadGuard&) = delete;
    RwSpinLockNativeReadGuard& operator=(const RwSpinLockNativeReadGuard&) = delete;

    RwSpinLockNativeReadGuard(RwSpinLockNativeReadGuard&&) = delete;
    RwSpinLockNativeReadGuard& operator=(RwSpinLockNativeReadGuard&&) = delete;

    ~RwSpinLockNativeReadGuard() {
        if (lock_) {
            rwspinlock_release_read(lock_);
        }
    }

private:
    rwspinlock_t* lock_ = nullptr;
};

class PerCpuRwSpinLockNativeReadGuard {
public:
    explicit PerCpuRwSpinLockNativeReadGuard(percpu_rwspinlock_t& lock)
        : lock_(&lock) {
        percpu_rwspinlock_acquire_read(lock_);
    }

    PerCpuRwSpinLockNativeReadGuard(const PerCpuRwSpinLockNativeReadGuard&) = delete;
    PerCpuRwSpinLockNativeReadGuard& operator=(const PerCpuRwSpinLockNativeReadGuard&) = delete;

    PerCpuRwSpinLockNativeReadGuard(PerCpuRwSpinLockNativeReadGuard&&) = delete;
    PerCpuRwSpinLockNativeReadGuard& operator=(PerCpuRwSpinLockNativeReadGuard&&) = delete;

    ~PerCpuRwSpinLockNativeReadGuard() {
        if (lock_) {
            percpu_rwspinlock_release_read(lock_);
        }
    }

private:
    percpu_rwspinlock_t* lock_ = nullptr;
};

class PerCpuRwSpinLockNativeReadSafeGuard {
public:
    explicit PerCpuRwSpinLockNativeReadSafeGuard(percpu_rwspinlock_t& lock)
        : lock_(&lock),
          flags_(percpu_rwspinlock_acquire_read_safe(lock_)) {
    }

    PerCpuRwSpinLockNativeReadSafeGuard(const PerCpuRwSpinLockNativeReadSafeGuard&) = delete;
    PerCpuRwSpinLockNativeReadSafeGuard& operator=(const PerCpuRwSpinLockNativeReadSafeGuard&) = delete;

    PerCpuRwSpinLockNativeReadSafeGuard(PerCpuRwSpinLockNativeReadSafeGuard&&) = delete;
    PerCpuRwSpinLockNativeReadSafeGuard& operator=(PerCpuRwSpinLockNativeReadSafeGuard&&) = delete;

    ~PerCpuRwSpinLockNativeReadSafeGuard() {
        if (lock_) {
            percpu_rwspinlock_release_read_safe(lock_, flags_);
        }
    }

private:
    percpu_rwspinlock_t* lock_ = nullptr;
    uint32_t flags_ = 0u;
};

class PerCpuRwSpinLockNativeWriteGuard {
public:
    explicit PerCpuRwSpinLockNativeWriteGuard(percpu_rwspinlock_t& lock)
        : lock_(&lock) {
        percpu_rwspinlock_acquire_write(lock_);
    }

    PerCpuRwSpinLockNativeWriteGuard(const PerCpuRwSpinLockNativeWriteGuard&) = delete;
    PerCpuRwSpinLockNativeWriteGuard& operator=(const PerCpuRwSpinLockNativeWriteGuard&) = delete;

    PerCpuRwSpinLockNativeWriteGuard(PerCpuRwSpinLockNativeWriteGuard&&) = delete;
    PerCpuRwSpinLockNativeWriteGuard& operator=(PerCpuRwSpinLockNativeWriteGuard&&) = delete;

    ~PerCpuRwSpinLockNativeWriteGuard() {
        if (lock_) {
            percpu_rwspinlock_release_write(lock_);
        }
    }

private:
    percpu_rwspinlock_t* lock_ = nullptr;
};

class PerCpuRwSpinLockNativeWriteSafeGuard {
public:
    explicit PerCpuRwSpinLockNativeWriteSafeGuard(percpu_rwspinlock_t& lock)
        : lock_(&lock),
          flags_(percpu_rwspinlock_acquire_write_safe(lock_)) {
    }

    PerCpuRwSpinLockNativeWriteSafeGuard(const PerCpuRwSpinLockNativeWriteSafeGuard&) = delete;
    PerCpuRwSpinLockNativeWriteSafeGuard& operator=(const PerCpuRwSpinLockNativeWriteSafeGuard&) = delete;

    PerCpuRwSpinLockNativeWriteSafeGuard(PerCpuRwSpinLockNativeWriteSafeGuard&&) = delete;
    PerCpuRwSpinLockNativeWriteSafeGuard& operator=(PerCpuRwSpinLockNativeWriteSafeGuard&&) = delete;

    ~PerCpuRwSpinLockNativeWriteSafeGuard() {
        if (lock_) {
            percpu_rwspinlock_release_write_safe(lock_, flags_);
        }
    }

private:
    percpu_rwspinlock_t* lock_ = nullptr;
    uint32_t flags_ = 0u;
};

class RwSpinLockNativeWriteSafeGuard {
public:
    explicit RwSpinLockNativeWriteSafeGuard(rwspinlock_t& lock)
        : lock_(&lock),
          flags_(rwspinlock_acquire_write_safe(lock_)) {
    }

    RwSpinLockNativeWriteSafeGuard(const RwSpinLockNativeWriteSafeGuard&) = delete;
    RwSpinLockNativeWriteSafeGuard& operator=(const RwSpinLockNativeWriteSafeGuard&) = delete;

    RwSpinLockNativeWriteSafeGuard(RwSpinLockNativeWriteSafeGuard&&) = delete;
    RwSpinLockNativeWriteSafeGuard& operator=(RwSpinLockNativeWriteSafeGuard&&) = delete;

    ~RwSpinLockNativeWriteSafeGuard() {
        if (lock_) {
            rwspinlock_release_write_safe(lock_, flags_);
        }
    }

private:
    rwspinlock_t* lock_ = nullptr;
    uint32_t flags_ = 0u;
};

class RwSpinLockNativeWriteGuard {
public:
    explicit RwSpinLockNativeWriteGuard(rwspinlock_t& lock)
        : lock_(&lock) {
        rwspinlock_acquire_write(lock_);
    }

    RwSpinLockNativeWriteGuard(const RwSpinLockNativeWriteGuard&) = delete;
    RwSpinLockNativeWriteGuard& operator=(const RwSpinLockNativeWriteGuard&) = delete;

    RwSpinLockNativeWriteGuard(RwSpinLockNativeWriteGuard&&) = delete;
    RwSpinLockNativeWriteGuard& operator=(RwSpinLockNativeWriteGuard&&) = delete;

    ~RwSpinLockNativeWriteGuard() {
        if (lock_) {
            rwspinlock_release_write(lock_);
        }
    }

private:
    rwspinlock_t* lock_ = nullptr;
};

}

#endif
