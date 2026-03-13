// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#include <hal/lock.h>

spinlock_qnode_t g_spinlock_qnodes[MAX_CPUS][SPINLOCK_QNODE_DEPTH];
volatile uint32_t g_spinlock_qnode_tops[MAX_CPUS];

void spinlock_acquire_native(spinlock_t* lock) {
    spinlock_acquire(lock);
}

uint32_t spinlock_acquire_safe_native(spinlock_t* lock) {
    return spinlock_acquire_safe(lock);
}

void spinlock_release_native(spinlock_t* lock) {
    spinlock_release(lock);
}

void spinlock_release_safe_native(spinlock_t* lock, uint32_t flags) {
    spinlock_release_safe(lock, flags);
}

int spinlock_try_acquire_native(spinlock_t* lock) {
    return spinlock_try_acquire(lock);
}
