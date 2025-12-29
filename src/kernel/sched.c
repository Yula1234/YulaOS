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
    uint32_t min_score = 0xFFFFFFFF;

    int active_cpus = 1 + ap_running_count;

    for (int i = 0; i < active_cpus; i++) {
        cpu_t* c = &cpus[i];
        
        uint32_t score = c->load_percent + 
                         (c->runq_count * 20) + 
                         (c->total_priority_weight);

        if (score < min_score) {
            min_score = score;
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
    
    target->total_priority_weight += t->priority;
    target->total_task_count++;

    runq_append(target, t);
    
    spinlock_release_safe(&target->lock, flags);
}

static task_t* pick_next_rr(cpu_t* cpu) {
    if (!cpu->runq_head) return 0;
    
    uint32_t count = cpu->runq_count;
    uint32_t inspected = 0;
    
    while (inspected < count && cpu->runq_head) {
        task_t* t = cpu->runq_head;
        
        cpu->runq_head = t->sched_next;
        if (cpu->runq_head) cpu->runq_head->sched_prev = 0;
        else cpu->runq_tail = 0;
        
        t->sched_next = 0;
        t->sched_prev = 0;
        t->is_queued = 0;
        
        if (t->state == TASK_RUNNABLE || t->state == TASK_RUNNING) {
            if (cpu->runq_tail) {
                cpu->runq_tail->sched_next = t;
                t->sched_prev = cpu->runq_tail;
                cpu->runq_tail = t;
            } else {
                cpu->runq_head = t;
                cpu->runq_tail = t;
            }
            t->is_queued = 1;
            return t;
        } else {
            cpu->runq_count--;
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
                if (next->pid == 0) {
                    __asm__ volatile("sti; hlt; cli");
                }
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
    
    if (target->total_priority_weight >= (int)t->priority)
        target->total_priority_weight -= t->priority;
    else 
        target->total_priority_weight = 0;

    if (target->total_task_count > 0)
        target->total_task_count--;

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
    sem->wait_head = 0;
    sem->wait_tail = 0;
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
        
        curr->sem_next = 0;
        if (sem->wait_tail) {
            sem->wait_tail->sem_next = curr;
            sem->wait_tail = curr;
        } else {
            sem->wait_head = curr;
            sem->wait_tail = curr;
        }
        
        curr->state = TASK_WAITING;
        
        spinlock_release_safe(&sem->lock, flags);
        
        sched_yield();
    }
}

void sem_signal(semaphore_t* sem) {
    uint32_t flags = spinlock_acquire_safe(&sem->lock);
    
    sem->count++;
    
    if (sem->wait_head) {
        task_t* t = sem->wait_head;
        sem->wait_head = t->sem_next;
        if (!sem->wait_head) sem->wait_tail = 0;
        
        t->state = TASK_RUNNABLE;

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

    if (sem->wait_head == t) {
        sem->wait_head = t->sem_next;
        if (!sem->wait_head) sem->wait_tail = 0;
    } else {
        task_t* curr = sem->wait_head;
        while (curr) {
            if (curr->sem_next == t) {
                curr->sem_next = t->sem_next;
                if (curr->sem_next == 0) sem->wait_tail = curr;
                break;
            }
            curr = curr->sem_next;
        }
    }
    
    t->blocked_on_sem = 0;
    t->sem_next = 0;
    
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