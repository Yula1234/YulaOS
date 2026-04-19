/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2026 Yula1234 */

#ifndef KERNEL_LOCKING_GUARDS_H
#define KERNEL_LOCKING_GUARDS_H

#include <lib/compiler.h>

#include "rwspinlock.h"
#include "spinlock.h"
#include "rwlock.h"
#include "mutex.h"

#ifndef __cplusplus

#define __cleanup_func(func) __attribute__((cleanup(func), unused))

#define _GUARD_CONCAT_IMPL(x, y) x##y
#define _GUARD_CONCAT(x, y) _GUARD_CONCAT_IMPL(x, y)
#define _GUARD_NAME(prefix) _GUARD_CONCAT(prefix, __COUNTER__)

___inline void _cleanup_spinlock(spinlock_t **lock) {
    spinlock_release(*lock);
}

#define guard_spinlock(lock_ptr) \
    spinlock_t* _GUARD_NAME(_sl_guard_) __cleanup_func(_cleanup_spinlock) = \
    (spinlock_acquire(lock_ptr), (lock_ptr))

typedef struct {
    spinlock_t *lock;
    uint32_t flags;
} _spinlock_safe_ctx_t;

___inline void _cleanup_spinlock_safe(_spinlock_safe_ctx_t *ctx) {
    spinlock_release_safe(ctx->lock, ctx->flags);
}

#define guard_spinlock_safe(lock_ptr) \
    _spinlock_safe_ctx_t _GUARD_NAME(_sl_safe_guard_) __cleanup_func(_cleanup_spinlock_safe) = \
    { .lock = (lock_ptr), .flags = spinlock_acquire_safe(lock_ptr) }

___inline void _cleanup_mutex(mutex_t **m) {
    mutex_unlock(*m);
}

#define guard_mutex(mutex_ptr) \
    mutex_t* _GUARD_NAME(_mtx_guard_) __cleanup_func(_cleanup_mutex) = \
    (mutex_lock(mutex_ptr), (mutex_ptr))

___inline void _cleanup_percpu_rwspin_write(percpu_rwspinlock_t **rw) {
    percpu_rwspinlock_release_write(*rw);
}

#define guard_percpu_rwspin_write(rw_ptr) \
    percpu_rwspinlock_t* _GUARD_NAME(_prw_w_guard_) __cleanup_func(_cleanup_percpu_rwspin_write) = \
    (percpu_rwspinlock_acquire_write(rw_ptr), (rw_ptr))

___inline void _cleanup_percpu_rwspin_read(percpu_rwspinlock_t **rw) {
    percpu_rwspinlock_release_read(*rw);
}

#define guard_percpu_rwspin_read(rw_ptr) \
    percpu_rwspinlock_t* _GUARD_NAME(_prw_r_guard_) __cleanup_func(_cleanup_percpu_rwspin_read) = \
    (percpu_rwspinlock_acquire_read(rw_ptr), (rw_ptr))

typedef struct {
    percpu_rwspinlock_t *lock;
    uint32_t flags;
} _percpu_rwspin_safe_ctx_t;

___inline void _cleanup_percpu_rwspin_write_safe(_percpu_rwspin_safe_ctx_t *ctx) {
    percpu_rwspinlock_release_write_safe(ctx->lock, ctx->flags);
}

#define guard_percpu_rwspin_write_safe(rw_ptr) \
    _percpu_rwspin_safe_ctx_t _GUARD_NAME(_prw_ws_guard_) __cleanup_func(_cleanup_percpu_rwspin_write_safe) = \
    { .lock = (rw_ptr), .flags = percpu_rwspinlock_acquire_write_safe(rw_ptr) }

___inline void _cleanup_percpu_rwspin_read_safe(_percpu_rwspin_safe_ctx_t *ctx) {
    percpu_rwspinlock_release_read_safe(ctx->lock, ctx->flags);
}

#define guard_percpu_rwspin_read_safe(rw_ptr) \
    _percpu_rwspin_safe_ctx_t _GUARD_NAME(_prw_rs_guard_) __cleanup_func(_cleanup_percpu_rwspin_read_safe) = \
    { .lock = (rw_ptr), .flags = percpu_rwspinlock_acquire_read_safe(rw_ptr) }

___inline void _cleanup_rwspin_write(rwspinlock_t **rw) {
    rwspinlock_release_write(*rw);
}

#define guard_rwspin_write(rw_ptr) \
    rwspinlock_t* _GUARD_NAME(_rw_w_guard_) __cleanup_func(_cleanup_rwspin_write) = \
    (rwspinlock_acquire_write(rw_ptr), (rw_ptr))

___inline void _cleanup_rwspin_read(rwspinlock_t **rw) {
    rwspinlock_release_read(*rw);
}

#define guard_rwspin_read(rw_ptr) \
    rwspinlock_t* _GUARD_NAME(_rw_r_guard_) __cleanup_func(_cleanup_rwspin_read) = \
    (rwspinlock_acquire_read(rw_ptr), (rw_ptr))

typedef struct {
    rwspinlock_t *lock;
    uint32_t flags;
} _rwspin_safe_ctx_t;

___inline void _cleanup_rwspin_write_safe(_rwspin_safe_ctx_t *ctx) {
    rwspinlock_release_write_safe(ctx->lock, ctx->flags);
}

#define guard_rwspin_write_safe(rw_ptr) \
    _rwspin_safe_ctx_t _GUARD_NAME(_rw_ws_guard_) __cleanup_func(_cleanup_rwspin_write_safe) = \
    { .lock = (rw_ptr), .flags = rwspinlock_acquire_write_safe(rw_ptr) }

___inline void _cleanup_rwspin_read_safe(_rwspin_safe_ctx_t *ctx) {
    rwspinlock_release_read_safe(ctx->lock, ctx->flags);
}

#define guard_rwspin_read_safe(rw_ptr) \
    _rwspin_safe_ctx_t _GUARD_NAME(_rw_rs_guard_) __cleanup_func(_cleanup_rwspin_read_safe) = \
    { .lock = (rw_ptr), .flags = rwspinlock_acquire_read_safe(rw_ptr) }

___inline void _cleanup_rwlock_write(rwlock_t **rw) {
    rwlock_release_write(*rw);
}

#define guard_rwlock_write(rw_ptr) \
    rwlock_t* _GUARD_NAME(_rwl_w_guard_) __cleanup_func(_cleanup_rwlock_write) = \
    (rwlock_acquire_write(rw_ptr), (rw_ptr))

___inline void _cleanup_rwlock_read(rwlock_t **rw) {
    rwlock_release_read(*rw);
}

#define guard_rwlock_read(rw_ptr) \
    rwlock_t* _GUARD_NAME(_rwl_r_guard_) __cleanup_func(_cleanup_rwlock_read) = \
    (rwlock_acquire_read(rw_ptr), (rw_ptr))

#endif

#endif