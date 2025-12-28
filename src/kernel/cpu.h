#ifndef KERNEL_CPU_H
#define KERNEL_CPU_H

#include <stdint.h>
#include <hal/lock.h>
#include "proc.h"

#define MAX_CPUS 32

typedef struct {
    int id;                 // LAPIC ID
    int index; 
    struct task* current_task;
    volatile int started; 
    
    struct task* runq_head;
    struct task* runq_tail;
    spinlock_t lock;
    struct task* idle_task;
    
    volatile uint32_t runq_count; 

    volatile uint64_t stat_total_ticks;
    volatile uint64_t stat_idle_ticks;

    volatile uint64_t snap_total_ticks;
    volatile uint64_t snap_idle_ticks;
    
    volatile uint32_t load_percent;

    volatile int total_priority_weight; 
    volatile int total_task_count;

} cpu_t;

extern cpu_t cpus[MAX_CPUS];
extern int cpu_count;
extern volatile int ap_running_count;

void cpu_init_system(void);
cpu_t* cpu_current(void);

#endif