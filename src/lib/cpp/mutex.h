#ifndef LIB_CPP_MUTEX_H
#define LIB_CPP_MUTEX_H

#include <lib/cpp/semaphore.h>

namespace kernel {

class Mutex {
public:
    Mutex()
        : sem_(1) {
    }

    Mutex(const Mutex&) = delete;
    Mutex& operator=(const Mutex&) = delete;

    Mutex(Mutex&&) = delete;
    Mutex& operator=(Mutex&&) = delete;

    void lock() {
        sem_.wait();
    }

    void unlock() {
        sem_.signal();
    }

    bool try_lock() {
        return sem_.try_acquire();
    }

private:
    Semaphore sem_;
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
