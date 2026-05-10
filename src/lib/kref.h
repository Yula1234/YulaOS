/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2026 Yula1234 */

#ifndef LIB_KREF_H
#define LIB_KREF_H

#include <kernel/panic.h>

#include <lib/compiler.h>
#include <lib/atomic.h>

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {

#define likely kernel::likely
#define unlikely kernel::unlikely

#endif

struct KRef;

/*
 * Callback signature for object destruction.
 * The enclosing structure can be retrieved using container_of() on the kref pointer.
 */
typedef void (*krefrcb_t)(struct KRef* kref);

typedef struct KRef {
    atomic_uint_t     count_;
    krefrcb_t release_cb_;
} kref_t;

/*
 * Initialize a kref and bind its finalizer callback.
 */
___inline void kref_init(kref_t* kref, krefrcb_t release_cb) {
    if (unlikely(!kref || !release_cb)) {
        return;
    }

    atomic_uint_store_explicit(&kref->count_, 1u, ATOMIC_RELEASE);

    kref->release_cb_ = release_cb;
}

/*
 * Read the current reference count.
 */
___inline uint32_t kref_read(const kref_t* kref) {
    return atomic_uint_load_explicit(&kref->count_, ATOMIC_RELAXED);
}

/*
 * Increment the reference count.
 */
___inline void kref_get(kref_t* kref) {
    (void)atomic_uint_fetch_add_explicit(&kref->count_, 1u, ATOMIC_RELAXED);
}

/*
 * Decrement the reference count and potentially trigger cleanup.
 *
 * Returns 1 if the reference count dropped to zero and the object was
 * released, 0 otherwise.
 */
___inline int kref_put(kref_t* kref) {
    if (unlikely(!kref))
        return 0;

    /*
     * We must ensure all memory writes made by this CPU to the object are
     * visible to the CPU that ultimately runs the destruction callback.
     * ATOMIC_ACQ_REL provides the necessary acquire-release semantics.
     */
    const uint32_t old_count = atomic_uint_fetch_sub_explicit(
        &kref->count_, 1u, ATOMIC_ACQ_REL
    );

    if (unlikely(old_count == 0u))
        panic("KREF: reference count underflow");

    if (old_count == 1u) {
        krefrcb_t release_cb = kref->release_cb_;

        if (likely(release_cb))
            release_cb(kref);

        return 1;
    }

    return 0;
}

#ifdef __cplusplus
}
#endif

#endif /* LIB_KREF_H */