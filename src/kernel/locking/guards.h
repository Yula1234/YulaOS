/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2026 Yula1234 */

#ifndef KERNEL_LOCKING_GUARDS_H
#define KERNEL_LOCKING_GUARDS_H

#include <lib/compiler.h>

#ifndef __cplusplus

#define __GUARD_CONCAT_IMPL(x, y) x##y
#define __GUARD_CONCAT(x, y) __GUARD_CONCAT_IMPL(x, y)
#define __GUARD_UNIQUE_ID(prefix) __GUARD_CONCAT(prefix, __COUNTER__)

#define CLASS(name, var) \
    class_##name##_t var __attribute__((cleanup(class_##name##_destructor))) = class_##name##_constructor

#define guard(name) CLASS(name, __GUARD_UNIQUE_ID(_guard_obj_))

#define scoped_guard(name, ...) \
    for (CLASS(name, __GUARD_UNIQUE_ID(_scope_))(__VA_ARGS__), \
         *__GUARD_UNIQUE_ID(_done_) = (void*)0; \
         !__GUARD_UNIQUE_ID(_done_); \
         __GUARD_UNIQUE_ID(_done_) = (void*)1)

#define DEFINE_GUARD(name, lock_type, lock_fn, unlock_fn) \
    typedef struct { lock_type *lock; } class_##name##_t; \
    ___inline void class_##name##_destructor(class_##name##_t *_T) { \
        if (_T->lock) { unlock_fn(_T->lock); } \
    } \
    ___inline class_##name##_t class_##name##_constructor(lock_type *l) { \
        class_##name##_t _t = { .lock = l }; \
        if (l) { lock_fn(l); } \
        return _t; \
    }

#define DEFINE_GUARD_SAFE(name, lock_type, lock_fn, unlock_fn) \
    typedef struct { lock_type *lock; uint32_t flags; } class_##name##_t; \
    ___inline void class_##name##_destructor(class_##name##_t *_T) { \
        if (_T->lock) { unlock_fn(_T->lock, _T->flags); } \
    } \
    ___inline class_##name##_t class_##name##_constructor(lock_type *l) { \
        class_##name##_t _t = { .lock = l, .flags = 0 }; \
        if (l) { _t.flags = lock_fn(l); } \
        return _t; \
    }

#endif /* __cplusplus */

#endif /* KERNEL_LOCKING_GUARDS_H */