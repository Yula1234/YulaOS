// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 Yula1234

#pragma once

#include <stdint.h>
#include <stddef.h>

#include <kernel/smp/cpu_limits.h>


#ifdef __cplusplus
extern "C" {
#endif

typedef struct rcu_ptr {
    void* ptr;
} rcu_ptr_t;

typedef struct rcu_head {
    struct rcu_head* next;
    void (*func)(struct rcu_head*);
} rcu_head_t;

void synchronize_rcu(void);
void call_rcu(rcu_head_t* head, void (*func)(rcu_head_t*));
void rcu_gc_task(void* arg);
void rcu_qs_count_inc(void);
uint32_t rcu_qs_count_read(int cpu_idx);

static inline void rcu_ptr_init(rcu_ptr_t* r) {
    __atomic_store_n(&r->ptr, NULL, __ATOMIC_RELAXED);
}

static inline void* rcu_ptr_read(const rcu_ptr_t* r) {
    return __atomic_load_n(&r->ptr, __ATOMIC_ACQUIRE);
}

static inline void rcu_ptr_assign(rcu_ptr_t* r, void* new_ptr) {
    __atomic_store_n(&r->ptr, new_ptr, __ATOMIC_RELEASE);
}

static inline void rcu_read_lock(void) {
    __asm__ volatile("" ::: "memory");
}

static inline void rcu_read_unlock(void) {
    __asm__ volatile("" ::: "memory");
}


#ifdef __cplusplus
}

namespace kernel {

inline void rcu_read_lock() {
    ::rcu_read_lock();
}

inline void rcu_read_unlock() {
    ::rcu_read_unlock();
}

inline void synchronize_rcu() {
    ::synchronize_rcu();
}

template <typename T>
class RcuPtr {
public:
    RcuPtr() = default;
    explicit RcuPtr(T* ptr) {
        rcu_ptr_assign(&ptr_, ptr);
    }

    T* read() const {
        return static_cast<T*>(rcu_ptr_read(&ptr_));
    }

    void assign(T* new_ptr) {
        rcu_ptr_assign(&ptr_, new_ptr);
    }

private:
    rcu_ptr_t ptr_{};
};

}
#endif
