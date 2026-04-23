#ifndef LIB_CPP_MUTEX_H
#define LIB_CPP_MUTEX_H

#include <kernel/locking/mutex.h>

namespace kernel {

class Mutex {
public:
    Mutex() {
        mutex_init(&mut_);
    }

    Mutex(const Mutex&) = delete;
    Mutex& operator=(const Mutex&) = delete;

    Mutex(Mutex&&) = delete;
    Mutex& operator=(Mutex&&) = delete;

    void lock() {
        mutex_lock(&mut_);
    }

    void unlock() {
        mutex_unlock(&mut_);
    }

    bool try_lock() {
        return mutex_try_lock(&mut_);
    }

private:
    mutex_t mut_;
};

class MutexGuard {
public:
    explicit MutexGuard(Mutex& mutex)
        : mutex_(&mutex) {
        mutex_->lock();
    }

    MutexGuard(const MutexGuard&) = delete;
    MutexGuard& operator=(const MutexGuard&) = delete;

    MutexGuard(MutexGuard&&) = delete;
    MutexGuard& operator=(MutexGuard&&) = delete;

    ~MutexGuard() {
        if (mutex_) {
            mutex_->unlock();
        }
    }

private:
    Mutex* mutex_;
};

}

#endif
