/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2026 Yula1234 */

#pragma once

#include <kernel/smp/cpu_limits.h>
#include <kernel/smp/cpu.h>
#include <kernel/smp/mb.h>

#include <lib/compiler.h>

#include <stdint.h>
#include <stddef.h>

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

void rcu_init_workers(void);

void call_rcu(rcu_head_t* head, void (*func)(rcu_head_t*));

void rcu_process_local(void);

___inline void rcu_ptr_init(rcu_ptr_t* r) {
    __atomic_store_n(&r->ptr, NULL, __ATOMIC_RELAXED);
}

___inline void* rcu_ptr_read(const rcu_ptr_t* r) {
    return __atomic_load_n(&r->ptr, __ATOMIC_ACQUIRE);
}

___inline void rcu_ptr_assign(rcu_ptr_t* r, void* new_ptr) {
    __atomic_store_n(&r->ptr, new_ptr, __ATOMIC_RELEASE);
}

___inline void rcu_read_lock(void) {
    compiler_mb();
}

___inline void rcu_read_unlock(void) {
    compiler_mb();
}

___inline uint32_t rcu_qs_count_read(int cpu_idx) {
    return __atomic_load_n(&cpus[cpu_idx].rcu_qs_count, __ATOMIC_RELAXED);
}

___inline void rcu_qs_count_inc(void) {
    cpu_t* cpu = cpu_current();

    __asm__ volatile("addl $1, %0" : "+m" (cpu->rcu_qs_count) :: "memory"); /* x86 have strong memory model, no lock prefix */
}


#ifdef __cplusplus
}

namespace kernel {

class RcuReadGuard {
public:
    RcuReadGuard() {
        ::rcu_read_lock();
    }

    RcuReadGuard(const RcuReadGuard&) = delete;
    RcuReadGuard& operator=(const RcuReadGuard&) = delete;

    RcuReadGuard(RcuReadGuard&&) = delete;
    RcuReadGuard& operator=(RcuReadGuard&&) = delete;

    ~RcuReadGuard() {
        ::rcu_read_unlock();
    }
};

template <typename T>
class RcuPtr {
public:
    RcuPtr() = default;
    explicit RcuPtr(T* ptr) {
        rcu_ptr_assign(&ptr_, ptr);
    }

    ___always_inline T* read() const {
        return static_cast<T*>(rcu_ptr_read(&ptr_));
    }

    ___always_inline void assign(T* new_ptr) {
        rcu_ptr_assign(&ptr_, new_ptr);
    }

private:
    rcu_ptr_t ptr_{};
};

}
#endif
