// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 Yula1234

#include <kernel/rcu.h>
#include <kernel/smp/cpu.h>
#include <kernel/sched.h>
#include <hal/apic.h>
#include <hal/lock.h>
#include <lib/cpp/atomic.h>
#include <lib/cpp/semaphore.h>

static kernel::atomic<rcu_head_t*> g_rcu_queue_head{nullptr};
static kernel::atomic<uint32_t> g_rcu_queue_len{0u};

static kernel::Semaphore g_rcu_sem{};
static kernel::atomic<uint32_t> g_rcu_sem_ready{0u};

extern "C" volatile uint32_t timer_ticks;

extern "C" void call_rcu(rcu_head_t* head, void (*func)(rcu_head_t*)) {
    if (!head || !func) {
        return;
    }

    head->func = func;

    rcu_head_t* old_head = g_rcu_queue_head.load(kernel::memory_order::relaxed);
    do {
        head->next = old_head;
    } while (!g_rcu_queue_head.compare_exchange_weak(
        old_head, head,
        kernel::memory_order::release,
        kernel::memory_order::relaxed
    ));

    uint32_t len = g_rcu_queue_len.fetch_add(1u, kernel::memory_order::relaxed);
    if (len == 256u && g_rcu_sem_ready.load(kernel::memory_order::relaxed) != 0u) {
        g_rcu_sem.signal();
    }
}

extern "C" void rcu_gc_task(void* arg) {
    (void)arg;

    g_rcu_sem_ready.store(1u, kernel::memory_order::release);

    for (;;) {
        g_rcu_sem.wait_timeout(timer_ticks + 10u);

        rcu_head_t* list = g_rcu_queue_head.exchange(nullptr, kernel::memory_order::acquire);
        g_rcu_queue_len.store(0u, kernel::memory_order::relaxed);

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
    uint32_t snap[MAX_CPUS];

    cpu_t* me = cpu_current();

    for (int i = 0; i < MAX_CPUS; i++) {
        if (cpus[i].id != -1 && &cpus[i] != me) {
            snap[i] = __atomic_load_n(&cpus[i].rcu_qs_count, __ATOMIC_RELAXED);

            lapic_write(LAPIC_ICRHI, (uint32_t)cpus[i].id << 24);
            lapic_write(LAPIC_ICRLO, (uint32_t)IPI_TLB_VECTOR | 0x00004000u);
        }
    }

    for (int i = 0; i < MAX_CPUS; i++) {
        if (cpus[i].id == -1 || &cpus[i] == me) {
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