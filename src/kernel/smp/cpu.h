// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#ifndef KERNEL_CPU_H
#define KERNEL_CPU_H

#include <stdint.h>
#include <kernel/smp/cpu_limits.h>
#include <hal/lock.h>
#include <lib/rbtree.h>
#include <kernel/proc.h>

#ifdef __cplusplus
extern "C" {
#endif

struct rcu_head;

typedef struct {
    int id;                 // LAPIC ID
    int index; 
    struct task* current_task;
    struct task* prev_task_during_switch;
    volatile int started; 
    
    struct rb_root runq_root;
    struct task* runq_leftmost;

    struct rb_root sleep_root;
    struct task* sleep_leftmost;

    spinlock_t sleep_lock;
    volatile uint32_t sleep_next_wake_tick;
    
    spinlock_t lock;
    struct task* idle_task;
    
    volatile uint32_t runq_count; 

    volatile uint64_t stat_total_ticks;
    volatile uint64_t stat_idle_ticks;

    volatile uint64_t sched_ticks;

    volatile uint64_t snap_total_ticks;
    volatile uint64_t snap_idle_ticks;
    
    volatile uint32_t load_percent;

    volatile int total_priority_weight; 
    volatile int total_task_count;

    volatile uint32_t rcu_qs_count;

    struct rcu_head* rcu_queue;
    volatile uint32_t rcu_qlen;

} __cacheline_aligned cpu_t;

extern cpu_t cpus[MAX_CPUS];
extern int cpu_count;
extern volatile int ap_running_count;

void cpu_init_system(void);
cpu_t* cpu_current(void);

#ifdef __cplusplus
}
#endif

#endif