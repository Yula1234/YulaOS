/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2026 Yula1234 */

#include <lib/cpp/lock_guard.h>
#include <lib/cpp/atomic.h>
#include <lib/compiler.h>

#include <kernel/smp/cpu.h>
#include <kernel/rcu.h>

#include <hal/apic.h>

extern "C" void call_rcu(rcu_head_t* head, void (*func)(rcu_head_t*)) {
    if (!head || !func) {
        return;
    }

    head->func = func;

    cpu_t* cpu = cpu_current();

    kernel::ScopedIrqDisable irq_guard;

    head->next = cpu->rcu_queue;
    cpu->rcu_queue = head;

    __atomic_fetch_add(&cpu->rcu_qlen, 1u, __ATOMIC_RELAXED);
}

static void rcu_invoke_callbacks(rcu_head_t* list) {
    while (list) {
        rcu_head_t* next = list->next;
        void (*f)(rcu_head_t*) = list->func;

        list->next = nullptr;
        list->func = nullptr;

        f(list);
        list = next;
    }
}

void rcu_process_local(void) {
    cpu_t* cpu = cpu_current();

    if (cpu->rcu_gp_active) {
        bool all_passed = true;

        for (int i = 0; i < cpu_count; i++) {
            if (cpus[i].id == -1 || &cpus[i] == cpu) {
                continue;
            }

            if (__atomic_load_n(&cpus[i].in_kernel, __ATOMIC_ACQUIRE) == 0u) {
                continue;
            }

            uint32_t current_qs = __atomic_load_n(&cpus[i].rcu_qs_count, __ATOMIC_ACQUIRE);
            if (current_qs != cpu->rcu_qs_snapshot[i]) {
                continue;
            }

            all_passed = false;
            break;
        }

        if (all_passed) {
            rcu_head_t* ready_list = nullptr;

            {
                kernel::ScopedIrqDisable irq_guard;

                if (cpu->rcu_gp_active) {
                    ready_list = cpu->rcu_pending;
                    cpu->rcu_pending = nullptr;
                    cpu->rcu_gp_active = false;
                }
            }

            if (ready_list) {
                rcu_invoke_callbacks(ready_list);
            }
        }
    }

    if (!cpu->rcu_gp_active) {
        kernel::ScopedIrqDisable irq_guard;

        if (cpu->rcu_queue) {
            cpu->rcu_pending = cpu->rcu_queue;
            cpu->rcu_queue = nullptr;
            cpu->rcu_qlen = 0u;

            cpu->rcu_gp_active = true;

            for (int i = 0; i < cpu_count; i++) {
                cpu->rcu_qs_snapshot[i] = __atomic_load_n(&cpus[i].rcu_qs_count, __ATOMIC_RELAXED);
            }
        }
    }
}

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