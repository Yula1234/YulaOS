#include <arch/i386/context.h>
#include <arch/i386/paging.h>
#include <arch/i386/gdt.h>
#include <hal/lock.h>
#include <hal/io.h>
#include <hal/simd.h>
#include <drivers/vga.h>

#include "sched.h"
#include "cpu.h"

void sched_init(void) {}

static int get_best_cpu(void) {
    int best_cpu = 0;
    uint32_t min_load = 0xFFFFFFFF;

    int active_cpus = 1 + ap_running_count;

    for (int i = 0; i < active_cpus; i++) {
        if (cpus[i].runq_count < min_load) {
            min_load = cpus[i].runq_count;
            best_cpu = i;
        }
    }
    return best_cpu;
}

static void runq_append(cpu_t* cpu, task_t* t) {
    t->sched_next = 0;
    t->sched_prev = 0;
    
    if (!cpu->runq_head) {
        cpu->runq_head = t;
        cpu->runq_tail = t;
    } else {
        cpu->runq_tail->sched_next = t;
        t->sched_prev = cpu->runq_tail;
        cpu->runq_tail = t;
    }
    cpu->runq_count++;
}

void sched_add(task_t* t) {
    int target_cpu_idx = get_best_cpu();
     
    cpu_t* target = &cpus[target_cpu_idx];

    t->quantum = (t->priority + 1) * 3;
    t->ticks_left = t->quantum;
    t->assigned_cpu = target_cpu_idx;

    uint32_t flags = spinlock_acquire_safe(&target->lock);
    
    runq_append(target, t);
    
    spinlock_release_safe(&target->lock, flags);
}

static task_t* pick_next_rr(cpu_t* cpu) {
    if (!cpu->runq_head) return 0;
    
    uint32_t inspected = 0;
    uint32_t count = cpu->runq_count;
    
    while (inspected < count) {
        task_t* t = cpu->runq_head;
        
        cpu->runq_head = t->sched_next;
        if (cpu->runq_head) cpu->runq_head->sched_prev = 0;
        else cpu->runq_tail = 0;
        
        t->sched_next = 0;
        if (cpu->runq_tail) {
            cpu->runq_tail->sched_next = t;
            t->sched_prev = cpu->runq_tail;
            cpu->runq_tail = t;
        } else {
            cpu->runq_head = t;
            cpu->runq_tail = t;
        }
        
        if (t->state == TASK_RUNNABLE) {
            return t;
        }
        
        inspected++;
    }
    return 0;
}

void sched_set_current(task_t* t) {
    cpu_t* cpu = cpu_current();
    cpu->current_task = t;
    
    uint32_t kstack_top = (uint32_t)t->kstack + t->kstack_size;
    kstack_top &= ~0xF; 
    
    tss_set_stack(cpu->index, kstack_top); 
    
    if (t->page_dir) paging_switch(t->page_dir);
    else paging_switch(kernel_page_directory);
}

void sched_start(task_t* first) {
    sched_set_current(first);
    first->state = TASK_RUNNING;
    fpu_restore(first->fpu_state);
    ctx_start(first->esp);
    for (;;) cpu_hlt();
}

void sched_yield(void) {
    __asm__ volatile("cli");
    
    cpu_t* me = cpu_current();
    task_t* prev = me->current_task;
    
    if (prev && prev->state == TASK_RUNNING) {
        prev->state = TASK_RUNNABLE;
        fpu_save(prev->fpu_state);
    }

    while (1) {
        uint32_t flags = spinlock_acquire_safe(&me->lock);
        task_t* next = pick_next_rr(me);
        spinlock_release_safe(&me->lock, flags);

        if (!next) {
            next = me->idle_task;
        }

        if (next) {
            if (next == prev) {
                __asm__ volatile("sti; hlt; cli");
                return;
            }
            
            me->current_task = next;
            sched_set_current(next);

            fpu_restore(next->fpu_state);

            if (prev) {
                ctx_switch(&prev->esp, next->esp);
            } else {
                ctx_start(next->esp);
            }
            return;
        }

        me->current_task = 0;

        __asm__ volatile("sti; hlt; cli");
    }
}

void sched_remove(task_t* t) {
    int cpu_idx = t->assigned_cpu;
    if (cpu_idx < 0 || cpu_idx >= MAX_CPUS) return;
    
    cpu_t* target = &cpus[cpu_idx];
    
    uint32_t flags = spinlock_acquire_safe(&target->lock);
    
    task_t* curr = target->runq_head;
    while (curr) {
        if (curr == t) {
            if (t->sched_prev) t->sched_prev->sched_next = t->sched_next;
            else target->runq_head = t->sched_next;

            if (t->sched_next) t->sched_next->sched_prev = t->sched_prev;
            else target->runq_tail = t->sched_prev;

            t->sched_next = 0;
            t->sched_prev = 0;
            if (target->runq_count > 0) target->runq_count--;
            break;
        }
        curr = curr->sched_next;
    }
    
    spinlock_release_safe(&target->lock, flags);
}