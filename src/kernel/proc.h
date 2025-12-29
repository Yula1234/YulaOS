#ifndef KERNEL_PROC_H
#define KERNEL_PROC_H

#include <arch/i386/idt.h>
#include <fs/vfs.h>
#include <hal/lock.h>
#include <lib/dlist.h>

#include <stdint.h>

typedef enum {
    TASK_UNUSED = 0,
    TASK_RUNNABLE,
    TASK_RUNNING,
    TASK_ZOMBIE,
    TASK_WAITING
} task_state_t;

#define NSIG 32

#define SIGINT  2
#define SIGILL  4
#define SIGSEGV 11
#define SIGTERM 15

typedef void (*sig_handler_t)(int);

typedef struct mmap_area {
    uint32_t vaddr_start;
    uint32_t vaddr_end;
    uint32_t file_offset;
    uint32_t length;
    uint32_t file_size; 
    
    struct vfs_node* file;
    
    struct mmap_area* next;
} mmap_area_t;

typedef enum {
    PRIO_IDLE = 0,
    PRIO_LOW  = 5,
    PRIO_USER = 10,
    PRIO_HIGH = 15,
    PRIO_GUI  = 20,
    PRIO_SUPER = 30,
} task_prio_t;

typedef struct task {
    uint32_t pid;
    int is_queued;
    task_state_t state;
    char name[32];

    struct task* next;
    struct task* prev;
    
    struct task* sched_next;
    struct task* sched_prev;

    struct task* hash_next;
    struct task* hash_prev;
    
    int assigned_cpu;

    uint32_t mem_pages;
    uint32_t wait_for_pid;
    uint32_t wake_tick;
    dlist_head_t sleep_node; 
    int is_blocked_on_kbd;

    uint32_t prog_break;
    uint32_t heap_start;
    
    uint32_t* esp;
    uint32_t* page_dir;
    
    void (*entry)(void*);
    void* arg;
    
    void* kstack;       
    uint32_t kstack_size;
    uint32_t stack_bottom;
    uint32_t stack_top;
    file_t fds[MAX_PROCESS_FDS];
    uint32_t cwd_inode;

    void* terminal;

    uint32_t pending_signals;
    sig_handler_t handlers[NSIG];
    registers_t signal_context; 
    int is_running_signal;

    uint32_t parent_pid;

    task_prio_t priority;
    uint32_t    quantum;
    uint32_t    ticks_left;

    uint8_t term_mode;

    mmap_area_t* mmap_list;
    uint32_t mmap_top;

    uint8_t fpu_state[512] __attribute__((aligned(16)));

    struct task* sem_next;
    void* blocked_on_sem;

    semaphore_t exit_sem; 
  
} task_t;

void proc_init(void);
task_t* proc_current(void);

task_t* proc_spawn_kthread(const char* name, task_prio_t prio, void (*entry)(void*), void* arg);
task_t* proc_spawn_elf(const char* filename, int argc, char** argv);

void proc_kill(task_t* t);
void proc_wait(uint32_t pid);
void reaper_task_func(void* arg);

task_t* proc_get_list_head();
uint32_t proc_task_count(void);
task_t* proc_task_at(uint32_t idx);
task_t* proc_create_idle(int cpu_index);
task_t* proc_find_by_pid(uint32_t pid);
void proc_sleep_remove(task_t* t);
void proc_check_sleepers(uint32_t current_tick);
void proc_sleep_add(task_t* t, uint32_t wake_tick);

#endif