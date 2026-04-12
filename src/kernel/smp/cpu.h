/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2025 Yula1234 */

#ifndef KERNEL_CPU_H
#define KERNEL_CPU_H

#include <kernel/smp/cpu_limits.h>

#include <lib/compiler.h>
#include <lib/rbtree.h>

#include <hal/lock.h>
#include <hal/align.h>

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

struct rcu_head;
struct task;

typedef struct {
    /* cacheline 1 */
    
    void* self __cacheline_aligned;
    
    int id; /* LAPIC ID */
    int index; /* Logical index */
    
    volatile int started;

    struct task* idle_task; 

    /* cacheline 2 */

    volatile uint32_t in_kernel __cacheline_aligned;
    volatile uint32_t rcu_qs_count;
    
    struct task* current_task;
    struct task* prev_task_during_switch;
    struct task* fpu_owner;

    volatile uint64_t sched_ticks;

    /* cacheline 3 */

    struct rcu_head* rcu_queue __cacheline_aligned;
    struct rcu_head* rcu_queue_tail;
    
    struct rcu_head* rcu_pending;
    struct rcu_head* rcu_pending_tail;
    
    volatile uint32_t rcu_qlen;
    
    bool rcu_gp_active;

    volatile uint64_t stat_total_ticks;
    volatile uint64_t stat_idle_ticks;

    /* cacheline 4 */

    spinlock_t lock __cacheline_aligned;

    volatile uint32_t runq_count;
    volatile uint32_t load_percent;
    
    volatile int total_priority_weight;
    volatile int total_task_count;
    
    struct rb_root runq_root;
    struct task* runq_leftmost;

    /* cacheline 5 */

    spinlock_t sleep_lock __cacheline_aligned;

    volatile uint32_t sleep_next_wake_tick;
    
    struct rb_root sleep_root;
    struct task* sleep_leftmost;

    /* cacheline 6 */

    volatile uint64_t snap_total_ticks __cacheline_aligned;
    volatile uint64_t snap_idle_ticks;

    uint32_t rcu_qs_snapshot[MAX_CPUS];

} __cacheline_aligned cpu_t;

extern cpu_t cpus[MAX_CPUS];

extern int cpu_count;

extern volatile int ap_running_count;

void cpu_init_system(void);

void cpu_setup_gs(int cpu_index);

___inline cpu_t* cpu_current(void) {
    cpu_t* p;
    __asm__ volatile("movl %%gs:0, %0" : "=r"(p));
    return p;
}

#ifdef __cplusplus
}
#endif

#endif