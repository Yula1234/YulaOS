#include <arch/i386/context.h>
#include <arch/i386/paging.h>
#include <arch/i386/gdt.h>

#include <hal/lock.h>
#include <hal/io.h>

#include "sched.h"

static task_t* runq_head = 0;
static task_t* runq_tail = 0;
static uint32_t runq_count = 0;

static task_t* current = 0;
static spinlock_t sched_lock;

extern task_t* current_task;

static void runq_append(task_t* t) {
    t->sched_next = 0;
    t->sched_prev = 0;
    
    if (!runq_head) {
        runq_head = t;
        runq_tail = t;
    } else {
        runq_tail->sched_next = t;
        t->sched_prev = runq_tail;
        runq_tail = t;
    }
    runq_count++;
}

static void runq_remove_node(task_t* t) {
    
    if (t->sched_prev) t->sched_prev->sched_next = t->sched_next;
    else runq_head = t->sched_next;

    if (t->sched_next) t->sched_next->sched_prev = t->sched_prev;
    else runq_tail = t->sched_prev;

    t->sched_next = 0;
    t->sched_prev = 0;
    if (runq_count > 0) runq_count--;
}

void sched_init(void) {
    runq_head = 0;
    runq_tail = 0;
    runq_count = 0;
    current = 0;
    spinlock_init(&sched_lock);
}

void sched_add(task_t* t) {
    uint32_t flags = spinlock_acquire_safe(&sched_lock);
    
    t->quantum = (t->priority + 1) * 3; 
    t->ticks_left = t->quantum;

    runq_append(t);
    
    spinlock_release_safe(&sched_lock, flags);
}

static task_t* pick_next_rr(void) {
    if (!runq_head) return 0;
    
    uint32_t inspected = 0;
    
    while (inspected < runq_count) {
        task_t* t = runq_head;
        runq_head = t->sched_next;
        if (runq_head) runq_head->sched_prev = 0;
        else runq_tail = 0;
        
        t->sched_next = 0;
        if (runq_tail) {
            runq_tail->sched_next = t;
            t->sched_prev = runq_tail;
            runq_tail = t;
        } else {
            runq_head = t;
            runq_tail = t;
        }
        
        if (t->state == TASK_RUNNABLE) {
            return t;
        }
        
        inspected++;
    }
    
    return 0;
}

void sched_set_current(task_t* t) {
    current = t;
    current_task = t;
    
    uint32_t kstack_top = (uint32_t)t->kstack + t->kstack_size;
    kstack_top &= ~0xF; 
    tss_set_stack(kstack_top); 
    
    if (t->page_dir) paging_switch(t->page_dir);
    else paging_switch(kernel_page_directory);
}

void sched_start(task_t* first) {
    current = first;
    sched_set_current(first);
    first->state = TASK_RUNNING;
    
    ctx_start(first->esp);
    for (;;) cpu_hlt();
}

void sched_yield(void) {
    __asm__ volatile("cli");
    if (!current) return;
    
    task_t* prev = current;
    if (prev && prev->state == TASK_RUNNING) prev->state = TASK_RUNNABLE;

    task_t* next = pick_next_rr();
    
    if (!next) {
        if (prev && prev->state != TASK_ZOMBIE && prev->state != TASK_UNUSED) {
            prev->state = TASK_RUNNING;
            return;
        }
        
        __asm__ volatile("sti; hlt");
        return; 
    }

    next->state = TASK_RUNNING;
    current = next;
    sched_set_current(next);

    if (prev == next) return;

    ctx_switch(&prev->esp, next->esp);
}

void sched_remove(task_t* t) {
    uint32_t flags = spinlock_acquire_safe(&sched_lock);
    
    task_t* curr = runq_head;
    while (curr) {
        if (curr == t) {
            runq_remove_node(t);
            break;
        }
        curr = curr->sched_next;
    }
    
    spinlock_release_safe(&sched_lock, flags);
}