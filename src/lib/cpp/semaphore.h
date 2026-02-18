#ifndef LIB_CPP_SEMAPHORE_H
#define LIB_CPP_SEMAPHORE_H

#include <hal/lock.h>

namespace kernel {

class Semaphore {
public:
    Semaphore() {
        sem_init(&sem_, 0);
    }

    explicit Semaphore(int init_count) {
        sem_init(&sem_, init_count);
    }

    Semaphore(const Semaphore&) = delete;
    Semaphore& operator=(const Semaphore&) = delete;

    Semaphore(Semaphore&&) = delete;
    Semaphore& operator=(Semaphore&&) = delete;

    void init(int init_count) {
        sem_init(&sem_, init_count);
    }

    void wait() {
        sem_wait(&sem_);
    }

    void signal() {
        sem_signal(&sem_);
    }

    void signal_all() {
        sem_signal_all(&sem_);
    }

    bool try_acquire() {
        return sem_try_acquire(&sem_) != 0;
    }

    semaphore_t* raw() {
        return &sem_;
    }

    const semaphore_t* raw() const {
        return &sem_;
    }

private:
    semaphore_t sem_{};
};

class SemaphoreGuard {
public:
    explicit SemaphoreGuard(Semaphore& sem)
        : sem_(&sem) {
        sem_->wait();
    }

    SemaphoreGuard(const SemaphoreGuard&) = delete;
    SemaphoreGuard& operator=(const SemaphoreGuard&) = delete;

    SemaphoreGuard(SemaphoreGuard&&) = delete;
    SemaphoreGuard& operator=(SemaphoreGuard&&) = delete;

    ~SemaphoreGuard() {
        if (sem_) {
            sem_->signal();
        }
    }

private:
    Semaphore* sem_;
};

}

#endif
