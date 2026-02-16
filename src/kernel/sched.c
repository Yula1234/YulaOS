// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#include <arch/i386/context.h>
#include <arch/i386/paging.h>
#include <arch/i386/gdt.h>
#include <hal/lock.h>
#include <hal/io.h>
#include <hal/simd.h>
#include <drivers/vga.h>
#include <lib/rbtree.h>

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
    int nice = 10 - (int)prio;
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
    int active_cpus = 1 + ap_running_count;
    int cache_valid = (cached_best_cpu >= 0 && 
                       cache_tick != 0 &&
                       current_tick - cache_tick < CPU_CACHE_INVALIDATE_TICKS);
    
    if (cache_valid && active_cpus <= 1) {
        int result = cached_best_cpu;
        spinlock_release_safe(&cpu_cache_lock, flags);
        return result;
    }
    
    spinlock_release_safe(&cpu_cache_lock, flags);
    
    int best_cpu = 0;
    uint32_t min_score = 0xFFFFFFFF;

    cpu_t* me = cpu_current();
    int start_cpu = me ? me->index : 0;

    for (int ofs = 1; ofs <= active_cpus; ofs++) {
        int i = (start_cpu + ofs) % active_cpus;
        cpu_t* c = &cpus[i];
        
        uint32_t load = c->load_percent;
        uint32_t runq = c->runq_count;
        int weight = c->total_priority_weight;
        
        uint32_t score = load + (runq * 20) + (weight > 0 ? weight : 0);

        if (i == 0 && active_cpus > 1) {
            score += 25;
        }

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

static void enqueue_task(cpu_t* cpu, task_t* p) {
    struct rb_node **link = &cpu->runq_root.rb_node;
    struct rb_node *parent = 0;
    struct task *entry;
    int leftmost = 1;

    while (*link) {
        parent = *link;
        entry = rb_entry(parent, struct task, rb_node);
        
        if (p->vruntime < entry->vruntime) {
            link = &parent->rb_left;
        } else {
            link = &parent->rb_right;
            leftmost = 0;
        }
    }

    rb_link_node(&p->rb_node, parent, link);
    rb_insert_color(&p->rb_node, &cpu->runq_root);
    
    if (leftmost)
        cpu->runq_leftmost = p;
        
    cpu->runq_count++;
}

static void dequeue_task(cpu_t* cpu, task_t* p) {
    if (cpu->runq_leftmost == p) {
        struct rb_node *next = rb_next(&p->rb_node);
        cpu->runq_leftmost = next ? rb_entry(next, struct task, rb_node) : 0;
    }

    rb_erase(&p->rb_node, &cpu->runq_root);
    cpu->runq_count--;
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

    uint32_t base_quantum;
    if (t->priority >= PRIO_GUI) {
        base_quantum = 8;
    } else if (t->priority >= PRIO_USER) {
        base_quantum = 4;
    } else {
        base_quantum = 2;
    }

    t->quantum = base_quantum;
    t->ticks_left = t->quantum;
    
    if (t->vruntime == 0) {
        uint64_t min_vruntime;
        
        if (target->runq_leftmost) {
            min_vruntime = target->runq_leftmost->vruntime;
        } else {
            min_vruntime = (uint64_t)target->sched_ticks * NICE_0_LOAD;
        }
        
        t->vruntime = min_vruntime;
    }
    
    t->exec_start = 0;
    
    target->total_priority_weight += t->priority;
    target->total_task_count++;

    enqueue_task(target, t);
    
    uint32_t cache_flags = spinlock_acquire_safe(&cpu_cache_lock);
    if (cached_best_cpu == target_cpu_idx) {
        cache_tick = 0;
    }
    spinlock_release_safe(&cpu_cache_lock, cache_flags);
    
    spinlock_release_safe(&target->lock, flags);
}

static task_t* pick_next_cfs(cpu_t* cpu) {
    task_t* left = cpu->runq_leftmost;
    
    if (!left) return 0;
    
    dequeue_task(cpu, left);
    left->is_queued = 0;
    
    return left;
}

void sched_set_current(task_t* t) {
    cpu_t* cpu = cpu_current();
    cpu->current_task = t;
    
    uint32_t kstack_top = (uint32_t)t->kstack + t->kstack_size;
    kstack_top &= ~0xF; 
    
    tss_set_stack(cpu->index, kstack_top); 
    
    if (t->mem && t->mem->page_dir) paging_switch(t->mem->page_dir);
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
    uint32_t eflags;
    __asm__ volatile("pushfl; popl %0" : "=r"(eflags));
    __asm__ volatile("cli");
    
    cpu_t* me = cpu_current();
    task_t* prev = me->current_task;
    
    if (prev && prev->state == TASK_RUNNING) {
        prev->state = TASK_RUNNABLE;
        fpu_save(prev->fpu_state);
        
        if (prev->exec_start > 0 && prev->pid != 0) {
            uint64_t delta_exec = me->sched_ticks - prev->exec_start;
            if (delta_exec > 0) {
                uint32_t weight = calc_weight(prev->priority);
                uint64_t delta_vruntime = calc_delta_vruntime(delta_exec, weight);
                prev->vruntime += delta_vruntime;
            }
        }
        prev->exec_start = 0;

        uint32_t flags = spinlock_acquire_safe(&me->lock);
        prev->is_queued = 1;
        enqueue_task(me, prev);
        spinlock_release_safe(&me->lock, flags);
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
                if (eflags & 0x200) __asm__ volatile("sti");
                return;
            }
            
            next->exec_start = me->sched_ticks;
            
            me->current_task = next;
            sched_set_current(next);

            fpu_restore(next->fpu_state);

            if (eflags & 0x200) __asm__ volatile("sti");

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

    if (t->is_queued) {
        dequeue_task(target, t);
        t->is_queued = 0;
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

void sem_signal_all(semaphore_t* sem) {
    uint32_t flags = spinlock_acquire_safe(&sem->lock);

    while (!dlist_empty(&sem->wait_list)) {
        task_t* t = container_of(sem->wait_list.next, task_t, sem_node);

        dlist_del(&t->sem_node);

        t->sem_node.next = 0;
        t->sem_node.prev = 0;
        t->blocked_on_sem = 0;

        sem->count++;

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
