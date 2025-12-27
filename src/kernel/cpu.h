#ifndef KERNEL_CPU_H
#define KERNEL_CPU_H

#include <stdint.h>
#include "proc.h"

#define MAX_CPUS 32

typedef struct {
    int id; 
    int index; 
    struct task* current_task;
    volatile int started; 
} cpu_t;

extern cpu_t cpus[MAX_CPUS];
extern int cpu_count;
extern volatile int ap_running_count;

void cpu_init_system(void);
cpu_t* cpu_current(void);

#endif