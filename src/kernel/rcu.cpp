/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2026 Yula1234 */

#include <lib/cpp/lock_guard.h>
#include <lib/cpp/atomic.h>
#include <lib/compiler.h>

#include <kernel/workqueue.h>
#include <kernel/smp/cpu.h>
#include <kernel/rcu.h>

#include <hal/align.h>
#include <hal/apic.h>

struct RcuCpuState {
    rcu_head_t* ready_list = nullptr;
    workqueue_t* wq = nullptr;
    work_struct_t work{};
};

__cacheline_aligned static RcuCpuState g_rcu_state[MAX_CPUS];

extern "C" void call_rcu(rcu_head_t* head, void (*func)(rcu_head_t*)) {
    if (!head || !func) {
        return;
    }

    head->func = func;

    cpu_t* cpu = cpu_current();

    kernel::ScopedIrqDisable irq_guard;

    head->next = cpu->rcu_queue;
    
    if (!cpu->rcu_queue) {
        cpu->rcu_queue_tail = head;
    }

    cpu->rcu_queue = head;

    __atomic_fetch_add(&cpu->rcu_qlen, 1u, __ATOMIC_RELAXED);
}

___inline void rcu_invoke_callbacks(rcu_head_t* list) {
    while (list) {
        rcu_head_t* next = list->next;

        void (*f)(rcu_head_t*) = list->func;

        if (next) {
            __builtin_prefetch(next, 0, 0); 
        }

        list->next = nullptr;
        list->func = nullptr;

        f(list);
        
        list = next;
    }
}

static void rcu_execute_callbacks_work(work_struct_t* work) {
    RcuCpuState* state = container_of(work, RcuCpuState, work);
    
    rcu_head_t* list = __atomic_exchange_n(&state->ready_list, nullptr, __ATOMIC_ACQ_REL);

    if (list) {
        rcu_invoke_callbacks(list);
    }
}

extern "C" volatile uint32_t timer_ticks;

void rcu_process_local(void) {
    cpu_t* cpu = cpu_current();

    if (!cpu->rcu_gp_active && !cpu->rcu_queue) {
        return;
    }

    if (cpu->rcu_gp_active) {
        if ((timer_ticks & 7) != 0) {
            return; 
        }

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
                all_passed = false;
                break;
            }
        }

        if (all_passed) {
            rcu_head_t* ready_list = nullptr;
            rcu_head_t* ready_tail = nullptr;

            {
                kernel::ScopedIrqDisable irq_guard;

                if (cpu->rcu_gp_active) {
                    ready_list = cpu->rcu_pending;
                    ready_tail = cpu->rcu_pending_tail;

                    cpu->rcu_pending = nullptr;
                    cpu->rcu_pending_tail = nullptr;
                    
                    cpu->rcu_gp_active = false;
                }
            }

            if (ready_list) {
                int idx = cpu->index;

                if (idx >= 0 && idx < MAX_CPUS) {
                    RcuCpuState& state = g_rcu_state[idx];
                    
                    rcu_head_t* old_head = __atomic_load_n(&state.ready_list, __ATOMIC_RELAXED);

                    do {
                        ready_tail->next = old_head;
                    } while (!__atomic_compare_exchange_n(
                        &state.ready_list, &old_head, ready_list, 
                        true, __ATOMIC_RELEASE, __ATOMIC_RELAXED
                    ));
                    
                    if (state.wq) {
                        queue_work(state.wq, &state.work);
                    }
                }
            }
        }
    }

    if (!cpu->rcu_gp_active) {
        kernel::ScopedIrqDisable irq_guard;

        if (cpu->rcu_queue) {
            cpu->rcu_pending = cpu->rcu_queue;
            cpu->rcu_pending_tail = cpu->rcu_queue_tail;
            
            cpu->rcu_queue = nullptr;
            cpu->rcu_queue_tail = nullptr;
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

extern "C" void rcu_init_workers(void) {
    for (int i = 0; i < cpu_count; i++) {
        g_rcu_state[i].wq = create_workqueue("rcu");

        init_work(&g_rcu_state[i].work, rcu_execute_callbacks_work);
    }
}