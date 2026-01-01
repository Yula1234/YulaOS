// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#include <arch/i386/context.h>
#include <arch/i386/paging.h>
#include <arch/i386/gdt.h>
#include <hal/lock.h>
#include <hal/io.h>
#include <hal/simd.h>
#include <drivers/vga.h>

#include "sched.h"
#include "cpu.h"

extern volatile uint32_t timer_ticks;

#define NICE_0_LOAD 1024
#define MIN_GRANULARITY 1
#define CPU_CACHE_INVALIDATE_TICKS 100 

static int cached_best_cpu = -1;
static uint32_t cache_tick = 0;
static spinlock_t cpu_cache_lock;

uint32_t calc_weight(task_prio_t prio) {
    int nice = (int)prio - 10;
    if (nice < -20) nice = -20;
    if (nice > 19) nice = 19;
    
    static const uint32_t prio_to_weight[40] = {
        88761, 71755, 56483, 46273, 36291,
        29154, 23254, 18705, 14949, 11916,
        9548, 7620, 6100, 4904, 3906,
        3121, 2501, 1991, 1586, 1277,
        1024, 820, 655, 526, 423,
        335, 272, 215, 172, 137,
        110, 87, 70, 56, 45,
        36, 29, 23, 18, 15
    };
    
    return prio_to_weight[nice + 20];
}

uint64_t calc_delta_vruntime(uint64_t delta_exec, uint32_t weight) {
    if (weight == 0) return delta_exec;
    return (delta_exec * NICE_0_LOAD) / weight;
}

void sched_init(void) {
    cached_best_cpu = -1;
    cache_tick = 0;
    spinlock_init(&cpu_cache_lock);
}

static int get_best_cpu(void) {
    uint32_t flags = spinlock_acquire_safe(&cpu_cache_lock);
    
    uint32_t current_tick = timer_ticks;
    int cache_valid = (cached_best_cpu >= 0 && 
                       cache_tick != 0 &&
                       current_tick - cache_tick < CPU_CACHE_INVALIDATE_TICKS);
    
    if (cache_valid) {
        int result = cached_best_cpu;
        spinlock_release_safe(&cpu_cache_lock, flags);
        return result;
    }
    
    spinlock_release_safe(&cpu_cache_lock, flags);
    
    int best_cpu = 0;
    uint32_t min_score = 0xFFFFFFFF;
    int active_cpus = 1 + ap_running_count;

    for (int i = 0; i < active_cpus; i++) {
        cpu_t* c = &cpus[i];
        
        uint32_t load = c->load_percent;
        uint32_t runq = c->runq_count;
        int weight = c->total_priority_weight;
        
        uint32_t score = load + (runq * 20) + (weight > 0 ? weight : 0);

        if (score < min_score) {
            min_score = score;
            best_cpu = i;
        }
    }
    
    flags = spinlock_acquire_safe(&cpu_cache_lock);
    cached_best_cpu = best_cpu;
    cache_tick = current_tick;
    spinlock_release_safe(&cpu_cache_lock, flags);
    
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
    if (t->assigned_cpu == -1) {
        t->assigned_cpu = get_best_cpu();
    }
    
    int target_cpu_idx = t->assigned_cpu;
    cpu_t* target = &cpus[target_cpu_idx];

    uint32_t flags = spinlock_acquire_safe(&target->lock);

    if (t->is_queued) {
        spinlock_release_safe(&target->lock, flags);
        return; 
    }

    t->is_queued = 1;

    t->quantum = (t->priority + 1) * 3;
    t->ticks_left = t->quantum;
    
    if (t->vruntime == 0) {
        task_t* min_task = 0;
        uint64_t min_vruntime = 0xFFFFFFFFFFFFFFFFULL;
        
        task_t* curr = target->runq_head;
        while (curr) {
            if (curr->state == TASK_RUNNABLE && curr->vruntime < min_vruntime) {
                min_vruntime = curr->vruntime;
                min_task = curr;
            }
            curr = curr->sched_next;
        }
        
        if (min_task) {
            t->vruntime = min_task->vruntime;
        } else {
            t->vruntime = timer_ticks * NICE_0_LOAD;
        }
    }
    
    t->exec_start = timer_ticks;
    
    target->total_priority_weight += t->priority;
    target->total_task_count++;

    runq_append(target, t);
    
    uint32_t cache_flags = spinlock_acquire_safe(&cpu_cache_lock);
    if (cached_best_cpu == target_cpu_idx) {
        cache_tick = 0;
    }
    spinlock_release_safe(&cpu_cache_lock, cache_flags);
    
    spinlock_release_safe(&target->lock, flags);
}

static task_t* pick_next_cfs(cpu_t* cpu) {
    if (!cpu->runq_head) return 0;
    
    task_t* best = 0;
    uint64_t min_vruntime = 0xFFFFFFFFFFFFFFFFULL;
    
    task_t* curr = cpu->runq_head;
    while (curr) {
        if ((curr->state == TASK_RUNNABLE || curr->state == TASK_RUNNING) && 
            curr->vruntime < min_vruntime) {
            min_vruntime = curr->vruntime;
            best = curr;
        }
        curr = curr->sched_next;
    }
    
    if (!best) return 0;
    
    if (cpu->runq_head == best) {
        cpu->runq_head = best->sched_next;
        if (cpu->runq_head) cpu->runq_head->sched_prev = 0;
        else cpu->runq_tail = 0;
    } else {
        if (best->sched_prev) best->sched_prev->sched_next = best->sched_next;
        if (best->sched_next) best->sched_next->sched_prev = best->sched_prev;
        else cpu->runq_tail = best->sched_prev;
    }
    
    best->sched_next = 0;
    best->sched_prev = 0;
    best->is_queued = 0;
    
    if (cpu->runq_tail) {
        cpu->runq_tail->sched_next = best;
        best->sched_prev = cpu->runq_tail;
        cpu->runq_tail = best;
    } else {
        cpu->runq_head = best;
        cpu->runq_tail = best;
    }
    best->is_queued = 1;
    
    return best;
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
        
        if (prev->exec_start > 0 && prev->pid != 0) {
            uint64_t delta_exec = timer_ticks - prev->exec_start;
            if (delta_exec > 0) {
                uint32_t weight = calc_weight(prev->priority);
                uint64_t delta_vruntime = calc_delta_vruntime(delta_exec, weight);
                prev->vruntime += delta_vruntime;
            }
        }
        prev->exec_start = 0;
    }

    while (1) {
        uint32_t flags = spinlock_acquire_safe(&me->lock);
        task_t* next = pick_next_cfs(me);
        spinlock_release_safe(&me->lock, flags);

        if (!next) {
            next = me->idle_task;
        }

        if (next) {
            if (next == prev) {
                if (next->pid == 0) {
                    __asm__ volatile("sti; hlt; cli");
                }
                return;
            }
            
            next->exec_start = timer_ticks;
            
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
    
    if (target->total_priority_weight >= (int)t->priority)
        target->total_priority_weight -= t->priority;
    else 
        target->total_priority_weight = 0;

    if (target->total_task_count > 0)
        target->total_task_count--;

    uint32_t cache_flags = spinlock_acquire_safe(&cpu_cache_lock);
    if (cached_best_cpu == cpu_idx) {
        cache_tick = 0;
    }
    spinlock_release_safe(&cpu_cache_lock, cache_flags);

    task_t* curr = target->runq_head;
    while (curr) {
        if (curr == t) {
            if (t->sched_prev) t->sched_prev->sched_next = t->sched_next;
            else target->runq_head = t->sched_next;

            if (t->sched_next) t->sched_next->sched_prev = t->sched_prev;
            else target->runq_tail = t->sched_prev;

            t->sched_next = 0;
            t->sched_prev = 0;
            t->is_queued = 0;
            if (target->runq_count > 0) target->runq_count--;
            break;
        }
        curr = curr->sched_next;
    }
    
    spinlock_release_safe(&target->lock, flags);
}

void sem_init(semaphore_t* sem, int init_count) {
    sem->count = init_count;
    spinlock_init(&sem->lock);
    dlist_init(&sem->wait_list);
}

int sem_try_acquire(semaphore_t* sem) {
    uint32_t flags = spinlock_acquire_safe(&sem->lock);
    if (sem->count > 0) {
        sem->count--;
        spinlock_release_safe(&sem->lock, flags);
        return 1;
    }
    spinlock_release_safe(&sem->lock, flags);
    return 0;
}

void sem_wait(semaphore_t* sem) {
    while (1) {
        uint32_t flags = spinlock_acquire_safe(&sem->lock);
        
        if (sem->count > 0) {
            sem->count--;
            task_t* curr = proc_current();
            curr->blocked_on_sem = 0;
            spinlock_release_safe(&sem->lock, flags);
            return;
        }
        
        task_t* curr = proc_current();
        
        curr->blocked_on_sem = (void*)sem;
        
        dlist_add_tail(&curr->sem_node, &sem->wait_list);
        
        curr->state = TASK_WAITING;
        
        spinlock_release_safe(&sem->lock, flags);
        
        sched_yield();
    }
}

void sem_signal(semaphore_t* sem) {
    uint32_t flags = spinlock_acquire_safe(&sem->lock);
    
    sem->count++;
    
    if (!dlist_empty(&sem->wait_list)) {
        task_t* t = container_of(sem->wait_list.next, task_t, sem_node);
        
        dlist_del(&t->sem_node);
        
        t->sem_node.next = 0;
        t->sem_node.prev = 0;
        t->blocked_on_sem = 0;
        
        if (t->state != TASK_ZOMBIE) {
            t->state = TASK_RUNNABLE;
            sched_add(t);
        }
    }
    
    spinlock_release_safe(&sem->lock, flags);
}

void sem_remove_task(task_t* t) {
    if (!t->blocked_on_sem) return;
    
    semaphore_t* sem = (semaphore_t*)t->blocked_on_sem;
    uint32_t flags = spinlock_acquire_safe(&sem->lock);
    
    if (t->blocked_on_sem != sem) {
        spinlock_release_safe(&sem->lock, flags);
        return;
    }

    if (t->sem_node.next && t->sem_node.prev) {
        dlist_del(&t->sem_node);
        t->sem_node.next = 0;
        t->sem_node.prev = 0;
    }
    
    t->blocked_on_sem = 0;
    
    spinlock_release_safe(&sem->lock, flags);
}

void rwlock_init(rwlock_t* rw) {
    sem_init(&rw->lock, 1);
    sem_init(&rw->write_sem, 1);
    rw->readers = 0;
}

void rwlock_acquire_read(rwlock_t* rw) {
    sem_wait(&rw->lock);
    rw->readers++;
    if (rw->readers == 1) {
        sem_wait(&rw->write_sem);
    }
    sem_signal(&rw->lock);
}

void rwlock_release_read(rwlock_t* rw) {
    sem_wait(&rw->lock);
    rw->readers--;
    if (rw->readers == 0) {
        sem_signal(&rw->write_sem);
    }
    sem_signal(&rw->lock);
}

void rwlock_acquire_write(rwlock_t* rw) {
    sem_wait(&rw->write_sem);
}

void rwlock_release_write(rwlock_t* rw) {
    sem_signal(&rw->write_sem);
}