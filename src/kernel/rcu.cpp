/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2026 Yula1234 */

#include <lib/cpp/lock_guard.h>
#include <lib/cpp/semaphore.h>
#include <lib/cpp/atomic.h>
#include <lib/compiler.h>

#include <kernel/smp/cpu.h>
#include <kernel/sched.h>
#include <kernel/rcu.h>

#include <hal/apic.h>

static kernel::Semaphore g_rcu_sem{};
static kernel::atomic<uint32_t> g_rcu_sem_ready{0u};

extern "C" volatile uint32_t timer_ticks;

extern "C" void call_rcu(rcu_head_t* head, void (*func)(rcu_head_t*)) {
    if (!head || !func) {
        return;
    }

    head->func = func;

    cpu_t* cpu = cpu_current();
    if (kernel::unlikely(!cpu)) {
        return;
    }

    kernel::ScopedIrqDisable irq_guard;

    rcu_head_t* old_head = __atomic_load_n(&cpu->rcu_queue, __ATOMIC_RELAXED);
    do {
        head->next = old_head;
    } while (!__atomic_compare_exchange_n(
        &cpu->rcu_queue, &old_head, head,
        true, __ATOMIC_RELEASE, __ATOMIC_RELAXED
    ));

    uint32_t len = __atomic_fetch_add(&cpu->rcu_qlen, 1u, __ATOMIC_RELAXED);
    if (len == 256u && g_rcu_sem_ready.load(kernel::memory_order::relaxed) != 0u) {
        g_rcu_sem.signal();
    }
}

extern "C" void rcu_gc_task(void* arg) {
    (void)arg;

    g_rcu_sem_ready.store(1u, kernel::memory_order::release);

    for (;;) {
        g_rcu_sem.wait_timeout(timer_ticks + 10u);

        rcu_head_t* list = nullptr;

        for (int i = 0; i < cpu_count; i++) {
            if (cpus[i].id == -1) {
                continue;
            }

            rcu_head_t* local = __atomic_exchange_n(&cpus[i].rcu_queue, nullptr, __ATOMIC_ACQUIRE);
            if (!local) {
                continue;
            }

            __atomic_store_n(&cpus[i].rcu_qlen, 0u, __ATOMIC_RELAXED);

            while (local) {
                rcu_head_t* next = local->next;
                local->next = list;
                list = local;
                local = next;
            }
        }

        if (!list) {
            continue;
        }

        synchronize_rcu();

        while (list) {
            rcu_head_t* next = list->next;
            void (*f)(rcu_head_t*) = list->func;

            list->next = nullptr;
            list->func = nullptr;

            f(list);
            list = next;
        }
    }
}

extern "C" void rcu_qs_count_inc(void);
extern "C" void synchronize_rcu(void) {
    const int n = cpu_count;
    uint32_t* snap = (uint32_t*)__builtin_alloca(sizeof(uint32_t) * n);
    uint8_t* need_wait = (uint8_t*)__builtin_alloca(sizeof(uint8_t) * n);

    cpu_t* me = cpu_current();

    for (int i = 0; i < n; i++) {
        need_wait[i] = 0u;

        if (cpus[i].id == -1 || &cpus[i] == me) {
            continue;
        }

        snap[i] = __atomic_load_n(&cpus[i].rcu_qs_count, __ATOMIC_RELAXED);

        if (__atomic_load_n(&cpus[i].in_kernel, __ATOMIC_ACQUIRE) == 0u) {
            continue;
        }

        need_wait[i] = 1u;

        lapic_write(LAPIC_ICRHI, (uint32_t)cpus[i].id << 24);
        lapic_write(LAPIC_ICRLO, (uint32_t)IPI_RCU_VECTOR | 0x00004000u);
    }

    for (int i = 0; i < n; i++) {
        if (cpus[i].id == -1 || &cpus[i] == me || need_wait[i] == 0u) {
            continue;
        }

        while (__atomic_load_n(&cpus[i].rcu_qs_count, __ATOMIC_RELAXED) == snap[i]) {
            rcu_qs_count_inc();

            __asm__ volatile("pause" ::: "memory");
        }
    }
}

extern "C" uint32_t rcu_qs_count_read(int cpu_idx) {
    return __atomic_load_n(&cpus[cpu_idx].rcu_qs_count, __ATOMIC_RELAXED);
}

extern "C" void rcu_qs_count_inc(void) {
    cpu_t* cpu = cpu_current();

    __atomic_fetch_add(&cpu->rcu_qs_count, 1, __ATOMIC_RELAXED);
}