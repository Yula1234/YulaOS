#ifndef LIB_CPP_RWLOCK_H
#define LIB_CPP_RWLOCK_H

#include <hal/lock.h>

namespace kernel {

class RwLock {
public:
    RwLock() {
        rwlock_init(&lock_);
    }

    RwLock(const RwLock&) = delete;
    RwLock& operator=(const RwLock&) = delete;

    RwLock(RwLock&&) = delete;
    RwLock& operator=(RwLock&&) = delete;

    void lock_read() {
        rwlock_acquire_read(&lock_);
    }

    void unlock_read() {
        rwlock_release_read(&lock_);
    }

    void lock_write() {
        rwlock_acquire_write(&lock_);
    }

    void unlock_write() {
        rwlock_release_write(&lock_);
    }

    rwlock_t* native_handle() {
        return &lock_;
    }

    const rwlock_t* native_handle() const {
        return &lock_;
    }

private:
    rwlock_t lock_{};
};

class ReadGuard {
public:
    explicit ReadGuard(RwLock& lock)
        : lock_(&lock) {
        lock_->lock_read();
    }

    ReadGuard(const ReadGuard&) = delete;
    ReadGuard& operator=(const ReadGuard&) = delete;

    ReadGuard(ReadGuard&&) = delete;
    ReadGuard& operator=(ReadGuard&&) = delete;

    ~ReadGuard() {
        if (lock_) {
            lock_->unlock_read();
        }
    }

private:
    RwLock* lock_ = nullptr;
};

class WriteGuard {
public:
    explicit WriteGuard(RwLock& lock)
        : lock_(&lock) {
        lock_->lock_write();
    }

    WriteGuard(const WriteGuard&) = delete;
    WriteGuard& operator=(const WriteGuard&) = delete;

    WriteGuard(WriteGuard&&) = delete;
    WriteGuard& operator=(WriteGuard&&) = delete;

    ~WriteGuard() {
        if (lock_) {
            lock_->unlock_write();
        }
    }

private:
    RwLock* lock_ = nullptr;
};

}

#endif
