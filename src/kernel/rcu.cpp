// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 Yula1234

#include <kernel/rcu.h>
#include <kernel/smp/cpu.h>
#include <kernel/sched.h>
#include <hal/apic.h>

extern "C" void rcu_qs_count_inc(void) {
    cpu_t* cpu = cpu_current();

    __atomic_fetch_add(&cpu->rcu_qs_count, 1, __ATOMIC_RELAXED);
}

extern "C" uint32_t rcu_qs_count_read(int cpu_idx) {
    return __atomic_load_n(&cpus[cpu_idx].rcu_qs_count, __ATOMIC_RELAXED);
}

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
