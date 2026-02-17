#ifndef LIB_CPP_LOCK_GUARD_H
#define LIB_CPP_LOCK_GUARD_H

#include <hal/lock.h>

#include <stdint.h>

namespace kernel {

class SpinLockSafeGuard {
public:
    explicit SpinLockSafeGuard(spinlock_t& lock)
        : lock_(&lock),
          flags_(spinlock_acquire_safe(lock_)) {
    }

    SpinLockSafeGuard(const SpinLockSafeGuard&) = delete;
    SpinLockSafeGuard& operator=(const SpinLockSafeGuard&) = delete;

    SpinLockSafeGuard(SpinLockSafeGuard&& other) = delete;
    SpinLockSafeGuard& operator=(SpinLockSafeGuard&& other) = delete;

    ~SpinLockSafeGuard() {
        if (lock_) {
            spinlock_release_safe(lock_, flags_);
        }
    }

private:
    spinlock_t* lock_;
    uint32_t flags_;
};

}

#endif
