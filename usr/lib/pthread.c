// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#include "pthread.h"
#include "stdlib.h"
#include "syscall.h"

#include <yos/proc.h>

#define PTHREAD_STATE_RUNNING 0u
#define PTHREAD_STATE_EXITED  1u
#define PTHREAD_STATE_JOINED  2u

typedef struct pthread_internal {
    void* (*start_routine)(void*);
    void* arg;
    void* retval;
    void* stack_base;
    uint32_t stack_size;
    int owns_stack;
    int detached;
    int pid;
    volatile uint32_t state;
    struct pthread_internal* next;
} pthread_internal_t;

static pthread_internal_t* pthread_list_head = 0;
static volatile uint32_t pthread_list_lock = 0;

static void pthread_list_lock_acquire(void) {
    while (__sync_lock_test_and_set(&pthread_list_lock, 1u)) {
    }
}

static void pthread_list_lock_release(void) {
    __sync_lock_release(&pthread_list_lock);
}

static void pthread_list_add(pthread_internal_t* t) {
    if (!t) return;
    pthread_list_lock_acquire();
    t->next = pthread_list_head;
    pthread_list_head = t;
    pthread_list_lock_release();
}

static void pthread_list_remove(pthread_internal_t* t) {
    if (!t) return;
    pthread_list_lock_acquire();
    pthread_internal_t** cur = &pthread_list_head;
    while (*cur) {
        if (*cur == t) {
            *cur = t->next;
            t->next = 0;
            break;
        }
        cur = &(*cur)->next;
    }
    pthread_list_lock_release();
}

static pthread_internal_t* pthread_list_find_by_pid(int pid) {
    pthread_internal_t* res = 0;
    pthread_list_lock_acquire();
    pthread_internal_t* cur = pthread_list_head;
    while (cur) {
        if (cur->pid == pid) {
            res = cur;
            break;
        }
        cur = cur->next;
    }
    pthread_list_lock_release();
    return res;
}

static uint32_t pthread_stack_default_size(void) {
    return PTHREAD_DEFAULT_STACK_SIZE;
}

static int pthread_validate_stack_size(uint32_t size) {
    if (size < PTHREAD_STACK_MIN) return -1;
    return 0;
}

static int pthread_futex_wait(volatile uint32_t* uaddr, uint32_t expected) {
    return syscall(54, (int)(uintptr_t)uaddr, (int)expected, 0);
}

static int pthread_futex_wake(volatile uint32_t* uaddr, uint32_t max_wake) {
    return syscall(55, (int)(uintptr_t)uaddr, (int)max_wake, 0);
}

static int pthread_prepare_stack(const pthread_attr_t* attr, void** out_base, uint32_t* out_size, int* out_owns) {
    uint32_t size = pthread_stack_default_size();
    void* base = 0;
    int owns = 0;

    if (attr) {
        if (attr->stack_size != 0) {
            size = attr->stack_size;
        }
        base = attr->stack_base;
        if (base && attr->stack_size == 0) {
            return -1;
        }
        if (attr->detached != PTHREAD_CREATE_JOINABLE) {
            return -1;
        }
    }

    if (pthread_validate_stack_size(size) != 0) {
        return -1;
    }

    if (!base) {
        base = malloc(size);
        if (!base) return -1;
        owns = 1;
    }

    *out_base = base;
    *out_size = size;
    *out_owns = owns;
    return 0;
}

static void pthread_cleanup_internal(pthread_internal_t* t) {
    if (!t) return;

    if (t->owns_stack && t->stack_base) {
        free(t->stack_base);
    }
    free(t);
}

static void pthread_finish_internal(pthread_internal_t* t, void* retval) {
    if (!t) {
        syscall(0, 0, 0, 0);
        for (;;) { }
    }

    t->retval = retval;
    __sync_synchronize();
    t->state = PTHREAD_STATE_EXITED;
    __sync_synchronize();
    pthread_futex_wake(&t->state, 0x7FFFFFFF);

    syscall(0, 0, 0, 0);
    for (;;) { }
}

static void pthread_trampoline(void* arg) {
    pthread_internal_t* t = (pthread_internal_t*)arg;
    void* res = 0;
    if (t) {
        t->pid = syscall(2, 0, 0, 0);
        pthread_list_add(t);
    }
    if (t && t->start_routine) {
        res = t->start_routine(t->arg);
    }
    pthread_finish_internal(t, res);
}

int pthread_attr_init(pthread_attr_t* attr) {
    if (!attr) return -1;
    attr->stack_base = 0;
    attr->stack_size = pthread_stack_default_size();
    attr->detached = PTHREAD_CREATE_JOINABLE;
    return 0;
}

int pthread_attr_destroy(pthread_attr_t* attr) {
    if (!attr) return -1;
    attr->stack_base = 0;
    attr->stack_size = 0;
    attr->detached = PTHREAD_CREATE_JOINABLE;
    return 0;
}

int pthread_attr_setstack(pthread_attr_t* attr, void* stack_base, uint32_t stack_size) {
    if (!attr || !stack_base) return -1;
    if (pthread_validate_stack_size(stack_size) != 0) return -1;
    attr->stack_base = stack_base;
    attr->stack_size = stack_size;
    return 0;
}

int pthread_attr_setstacksize(pthread_attr_t* attr, uint32_t stack_size) {
    if (!attr) return -1;
    if (pthread_validate_stack_size(stack_size) != 0) return -1;
    attr->stack_size = stack_size;
    return 0;
}

int pthread_attr_setdetachstate(pthread_attr_t* attr, int detached) {
    if (!attr) return -1;
    if (detached != PTHREAD_CREATE_JOINABLE) return -1;
    attr->detached = detached;
    return 0;
}

int pthread_create(pthread_t* thread, const pthread_attr_t* attr, void* (*start_routine)(void*), void* arg) {
    if (!thread || !start_routine) return -1;

    void* stack_base = 0;
    uint32_t stack_size = 0;
    int owns_stack = 0;
    if (pthread_prepare_stack(attr, &stack_base, &stack_size, &owns_stack) != 0) {
        return -1;
    }

    pthread_internal_t* t = (pthread_internal_t*)malloc(sizeof(pthread_internal_t));
    if (!t) {
        if (owns_stack && stack_base) {
            free(stack_base);
        }
        return -1;
    }

    t->start_routine = start_routine;
    t->arg = arg;
    t->retval = 0;
    t->stack_base = stack_base;
    t->stack_size = stack_size;
    t->owns_stack = owns_stack;
    t->detached = PTHREAD_CREATE_JOINABLE;
    t->pid = -1;
    t->state = PTHREAD_STATE_RUNNING;
    t->next = 0;

    uintptr_t stack_top_addr = (uintptr_t)stack_base + (uintptr_t)stack_size;
    if (stack_top_addr < (uintptr_t)stack_base) {
        pthread_cleanup_internal(t);
        return -1;
    }
    void* stack_top = (void*)stack_top_addr;
    int pid = yos_clone(pthread_trampoline, t, stack_top, stack_size);
    if (pid < 0) {
        pthread_cleanup_internal(t);
        return -1;
    }

    thread->pid = pid;
    thread->internal = t;
    return 0;
}

int pthread_join(pthread_t thread, void** retval) {
    pthread_internal_t* t = thread.internal;
    if (!t) return -1;
    if (t->detached) return -1;

    int wait_res = syscall(37, thread.pid, 0, 0);
    if (wait_res < 0) {
        while (t->state == PTHREAD_STATE_RUNNING) {
            pthread_futex_wait(&t->state, PTHREAD_STATE_RUNNING);
        }
    }

    if (!__sync_bool_compare_and_swap(&t->state, PTHREAD_STATE_EXITED, PTHREAD_STATE_JOINED)) {
        return -1;
    }

    if (retval) {
        *retval = t->retval;
    }

    pthread_list_remove(t);
    pthread_cleanup_internal(t);
    return 0;
}

int pthread_detach(pthread_t thread) {
    pthread_internal_t* t = thread.internal;
    if (!t) return -1;
    return -1;
}

void pthread_exit(void* retval) {
    int pid = syscall(2, 0, 0, 0);
    pthread_internal_t* t = pthread_list_find_by_pid(pid);
    pthread_finish_internal(t, retval);
}

pthread_t pthread_self(void) {
    pthread_t t;
    t.pid = syscall(2, 0, 0, 0);
    t.internal = 0;
    return t;
}

static int pthread_mutex_lock_slow(pthread_mutex_t* mutex) {
    for (;;) {
        uint32_t prev = __sync_lock_test_and_set(&mutex->value, 2u);
        if (prev == 0u) {
            return 0;
        }
        pthread_futex_wait(&mutex->value, 2u);
    }
}

int pthread_mutex_init(pthread_mutex_t* mutex) {
    if (!mutex) return -1;
    mutex->value = 0u;
    return 0;
}

int pthread_mutex_destroy(pthread_mutex_t* mutex) {
    if (!mutex) return -1;
    mutex->value = 0u;
    return 0;
}

int pthread_mutex_lock(pthread_mutex_t* mutex) {
    if (!mutex) return -1;
    if (__sync_bool_compare_and_swap(&mutex->value, 0u, 1u)) {
        return 0;
    }
    return pthread_mutex_lock_slow(mutex);
}

int pthread_mutex_trylock(pthread_mutex_t* mutex) {
    if (!mutex) return -1;
    if (__sync_bool_compare_and_swap(&mutex->value, 0u, 1u)) {
        return 0;
    }
    return -1;
}

int pthread_mutex_unlock(pthread_mutex_t* mutex) {
    if (!mutex) return -1;
    uint32_t prev = __sync_lock_test_and_set(&mutex->value, 0u);
    if (prev == 0u) {
        return -1;
    }
    if (prev == 2u) {
        pthread_futex_wake(&mutex->value, 1u);
    }
    return 0;
}

int pthread_cond_init(pthread_cond_t* cond) {
    if (!cond) return -1;
    cond->seq = 0u;
    return 0;
}

int pthread_cond_destroy(pthread_cond_t* cond) {
    if (!cond) return -1;
    cond->seq = 0u;
    return 0;
}

int pthread_cond_wait(pthread_cond_t* cond, pthread_mutex_t* mutex) {
    if (!cond || !mutex) return -1;
    uint32_t seq = cond->seq;
    if (pthread_mutex_unlock(mutex) != 0) {
        return -1;
    }
    pthread_futex_wait(&cond->seq, seq);
    return pthread_mutex_lock(mutex);
}

int pthread_cond_signal(pthread_cond_t* cond) {
    if (!cond) return -1;
    __sync_fetch_and_add(&cond->seq, 1u);
    pthread_futex_wake(&cond->seq, 1u);
    return 0;
}

int pthread_cond_broadcast(pthread_cond_t* cond) {
    if (!cond) return -1;
    __sync_fetch_and_add(&cond->seq, 1u);
    pthread_futex_wake(&cond->seq, 0x7FFFFFFFu);
    return 0;
}

int pthread_rwlock_init(pthread_rwlock_t* lock) {
    if (!lock) return -1;
    lock->state = 0;
    lock->writers_waiting = 0u;
    return 0;
}

int pthread_rwlock_destroy(pthread_rwlock_t* lock) {
    if (!lock) return -1;
    lock->state = 0;
    lock->writers_waiting = 0u;
    return 0;
}

int pthread_rwlock_rdlock(pthread_rwlock_t* lock) {
    if (!lock) return -1;
    for (;;) {
        int32_t state = lock->state;
        if (state >= 0 && lock->writers_waiting == 0u) {
            if (__sync_bool_compare_and_swap(&lock->state, state, state + 1)) {
                return 0;
            }
            continue;
        }
        if (state < 0) {
            pthread_futex_wait((volatile uint32_t*)&lock->state, (uint32_t)state);
            continue;
        }
        uint32_t waiting = lock->writers_waiting;
        if (waiting != 0u) {
            pthread_futex_wait(&lock->writers_waiting, waiting);
        }
    }
}

int pthread_rwlock_tryrdlock(pthread_rwlock_t* lock) {
    if (!lock) return -1;
    int32_t state = lock->state;
    if (state >= 0 && lock->writers_waiting == 0u) {
        if (__sync_bool_compare_and_swap(&lock->state, state, state + 1)) {
            return 0;
        }
    }
    return -1;
}

int pthread_rwlock_wrlock(pthread_rwlock_t* lock) {
    if (!lock) return -1;
    __sync_fetch_and_add(&lock->writers_waiting, 1u);
    for (;;) {
        if (__sync_bool_compare_and_swap(&lock->state, 0, -1)) {
            uint32_t remaining = __sync_fetch_and_sub(&lock->writers_waiting, 1u) - 1u;
            if (remaining == 0u) {
                pthread_futex_wake(&lock->writers_waiting, 0x7FFFFFFFu);
            }
            return 0;
        }
        int32_t state = lock->state;
        pthread_futex_wait((volatile uint32_t*)&lock->state, (uint32_t)state);
    }
}

int pthread_rwlock_trywrlock(pthread_rwlock_t* lock) {
    if (!lock) return -1;
    if (__sync_bool_compare_and_swap(&lock->state, 0, -1)) {
        return 0;
    }
    return -1;
}

int pthread_rwlock_unlock(pthread_rwlock_t* lock) {
    if (!lock) return -1;
    int32_t state = lock->state;
    if (state == -1) {
        if (!__sync_bool_compare_and_swap(&lock->state, -1, 0)) {
            return -1;
        }
        pthread_futex_wake((volatile uint32_t*)&lock->state, 0x7FFFFFFFu);
        return 0;
    }
    if (state > 0) {
        int32_t prev = __sync_fetch_and_sub(&lock->state, 1);
        int32_t next = prev - 1;
        if (next == 0) {
            pthread_futex_wake((volatile uint32_t*)&lock->state, 0x7FFFFFFFu);
        }
        return 0;
    }
    return -1;
}

int pthread_spin_init(pthread_spinlock_t* lock) {
    if (!lock) return -1;
    lock->value = 0u;
    return 0;
}

int pthread_spin_destroy(pthread_spinlock_t* lock) {
    if (!lock) return -1;
    lock->value = 0u;
    return 0;
}

int pthread_spin_lock(pthread_spinlock_t* lock) {
    if (!lock) return -1;
    while (__sync_lock_test_and_set(&lock->value, 1u)) {
        while (lock->value) {
        }
    }
    return 0;
}

int pthread_spin_trylock(pthread_spinlock_t* lock) {
    if (!lock) return -1;
    if (__sync_bool_compare_and_swap(&lock->value, 0u, 1u)) {
        return 0;
    }
    return -1;
}

int pthread_spin_unlock(pthread_spinlock_t* lock) {
    if (!lock) return -1;
    __sync_lock_release(&lock->value);
    return 0;
}

int pthread_barrier_init(pthread_barrier_t* barrier, uint32_t count) {
    if (!barrier || count == 0u) return -1;
    barrier->threshold = count;
    barrier->count = 0u;
    barrier->seq = 0u;
    return 0;
}

int pthread_barrier_destroy(pthread_barrier_t* barrier) {
    if (!barrier) return -1;
    barrier->threshold = 0u;
    barrier->count = 0u;
    barrier->seq = 0u;
    return 0;
}

int pthread_barrier_wait(pthread_barrier_t* barrier) {
    if (!barrier || barrier->threshold == 0u) return -1;
    uint32_t seq = barrier->seq;
    uint32_t count = __sync_add_and_fetch(&barrier->count, 1u);
    if (count == barrier->threshold) {
        __sync_lock_test_and_set(&barrier->count, 0u);
        __sync_fetch_and_add(&barrier->seq, 1u);
        pthread_futex_wake(&barrier->seq, 0x7FFFFFFFu);
        return PTHREAD_BARRIER_SERIAL_THREAD;
    }
    while (barrier->seq == seq) {
        pthread_futex_wait(&barrier->seq, seq);
    }
    return 0;
}
