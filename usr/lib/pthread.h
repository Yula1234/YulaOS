// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#ifndef _PTHREAD_H
#define _PTHREAD_H

#include <stdint.h>
#include <stddef.h>

#define PTHREAD_CREATE_JOINABLE 0
#define PTHREAD_CREATE_DETACHED 1

#define PTHREAD_STACK_MIN 16384u
#define PTHREAD_DEFAULT_STACK_SIZE 65536u

typedef struct pthread_internal pthread_internal_t;

typedef struct {
    int pid;
    pthread_internal_t* internal;
} pthread_t;

typedef struct {
    void* stack_base;
    uint32_t stack_size;
    int detached;
} pthread_attr_t;

typedef struct {
    volatile uint32_t value;
} pthread_mutex_t;

typedef struct {
    volatile uint32_t seq;
} pthread_cond_t;

typedef struct {
    volatile int32_t state;
    volatile uint32_t writers_waiting;
} pthread_rwlock_t;

typedef struct {
    volatile uint32_t value;
} pthread_spinlock_t;

typedef struct {
    uint32_t threshold;
    volatile uint32_t count;
    volatile uint32_t seq;
} pthread_barrier_t;

#define PTHREAD_MUTEX_INITIALIZER { 0u }
#define PTHREAD_COND_INITIALIZER { 0u }
#define PTHREAD_RWLOCK_INITIALIZER { 0, 0u }
#define PTHREAD_SPINLOCK_INITIALIZER { 0u }

#define PTHREAD_BARRIER_SERIAL_THREAD 1

int pthread_attr_init(pthread_attr_t* attr);
int pthread_attr_destroy(pthread_attr_t* attr);
int pthread_attr_setstack(pthread_attr_t* attr, void* stack_base, uint32_t stack_size);
int pthread_attr_setstacksize(pthread_attr_t* attr, uint32_t stack_size);
int pthread_attr_setdetachstate(pthread_attr_t* attr, int detached);

int pthread_create(pthread_t* thread, const pthread_attr_t* attr, void* (*start_routine)(void*), void* arg);
int pthread_join(pthread_t thread, void** retval);
int pthread_detach(pthread_t thread);
void pthread_exit(void* retval);
pthread_t pthread_self(void);

int pthread_mutex_init(pthread_mutex_t* mutex);
int pthread_mutex_destroy(pthread_mutex_t* mutex);
int pthread_mutex_lock(pthread_mutex_t* mutex);
int pthread_mutex_trylock(pthread_mutex_t* mutex);
int pthread_mutex_unlock(pthread_mutex_t* mutex);

int pthread_cond_init(pthread_cond_t* cond);
int pthread_cond_destroy(pthread_cond_t* cond);
int pthread_cond_wait(pthread_cond_t* cond, pthread_mutex_t* mutex);
int pthread_cond_signal(pthread_cond_t* cond);
int pthread_cond_broadcast(pthread_cond_t* cond);

int pthread_rwlock_init(pthread_rwlock_t* lock);
int pthread_rwlock_destroy(pthread_rwlock_t* lock);
int pthread_rwlock_rdlock(pthread_rwlock_t* lock);
int pthread_rwlock_tryrdlock(pthread_rwlock_t* lock);
int pthread_rwlock_wrlock(pthread_rwlock_t* lock);
int pthread_rwlock_trywrlock(pthread_rwlock_t* lock);
int pthread_rwlock_unlock(pthread_rwlock_t* lock);

int pthread_spin_init(pthread_spinlock_t* lock);
int pthread_spin_destroy(pthread_spinlock_t* lock);
int pthread_spin_lock(pthread_spinlock_t* lock);
int pthread_spin_trylock(pthread_spinlock_t* lock);
int pthread_spin_unlock(pthread_spinlock_t* lock);

int pthread_barrier_init(pthread_barrier_t* barrier, uint32_t count);
int pthread_barrier_destroy(pthread_barrier_t* barrier);
int pthread_barrier_wait(pthread_barrier_t* barrier);

#endif
