// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#include <lib/string.h>
#include <arch/i386/gdt.h>
#include "cpu.h"

cpu_t cpus[MAX_CPUS];
int cpu_count = 0;

void cpu_init_system(void) {
    memset(cpus, 0, sizeof(cpus));
    for(int i = 0; i < MAX_CPUS; i++) {
        cpus[i].id = -1;
        cpus[i].index = i;
        cpus[i].started = 0;

        cpus[i].fpu_owner = 0;

        cpus[i].stat_total_ticks = 0;
        cpus[i].stat_idle_ticks = 0;

        cpus[i].sched_ticks = 1;

        cpus[i].snap_total_ticks = 0;
        cpus[i].snap_idle_ticks = 0;
        cpus[i].load_percent = 0;

        cpus[i].total_priority_weight = 0;
        cpus[i].total_task_count = 0;

        cpus[i].in_kernel = 0;

        cpus[i].rcu_queue = 0;
        cpus[i].rcu_qlen = 0;
        
        cpus[i].runq_root = RB_ROOT;
        cpus[i].runq_leftmost = 0;

        cpus[i].sleep_root = RB_ROOT;
        cpus[i].sleep_leftmost = 0;
        cpus[i].sleep_next_wake_tick = 0xFFFFFFFFu;
        spinlock_init(&cpus[i].sleep_lock);

        cpus[i].runq_count = 0;
        spinlock_init(&cpus[i].lock);
    }
}

void cpu_setup_gs(int cpu_index) {
    if (cpu_index >= 0 && cpu_index < MAX_CPUS) {
        uint16_t selector = ((GDT_CPU_DATA_BASE + cpu_index) << 3) | 3;
        __asm__ volatile("mov %0, %%gs" : : "r"(selector));
    }
}