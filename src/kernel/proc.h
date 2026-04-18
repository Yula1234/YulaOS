/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2025 Yula1234 */

#ifndef KERNEL_PROC_H
#define KERNEL_PROC_H

#include <lib/compiler.h>
#include <lib/rbtree.h>
#include <lib/dlist.h>
#include <lib/maple_tree.h>

#include <kernel/smp/cpu.h>
#include <kernel/rcu.h>

#include <hal/align.h>
#include <hal/lock.h>

#include <arch/i386/idt.h>

#include <yos/proc.h>

#include <fs/vfs.h>

#include <mm/vma.h>

#include <stdint.h>

#define KSTACK_SIZE 32768

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    TASK_UNUSED = 0,
    TASK_RUNNABLE,
    TASK_RUNNING,
    TASK_STOPPED,
    TASK_ZOMBIE,
    TASK_WAITING
} task_state_t;

#define NSIG 32

#define SIGINT  2
#define SIGQUIT 3
#define SIGILL  4
#define SIGCONT 18
#define SIGTSTP 20
#define SIGTTIN 21
#define SIGTTOU 22
#define SIGSEGV 11
#define SIGTERM 15

#define MAP_SHARED  VMA_MAP_SHARED
#define MAP_PRIVATE VMA_MAP_PRIVATE
#define MAP_STACK   VMA_MAP_STACK

typedef void (*sig_handler_t)(int);

typedef vma_region_t mmap_area_t;

typedef struct proc_mem {
    uint32_t* page_dir;
    
    uint32_t prog_break;
    uint32_t heap_start;

    spinlock_t pt_lock;

    rwspinlock_t mmap_lock;
    maple_tree_t mmap_mt;

    vma_region_t* mmap_cache;
    
    uint32_t mmap_top;
    uint32_t free_area_cache;
    
    uint32_t mem_pages;

    uint32_t leader_pid;
    uint32_t refcount;

    volatile uint32_t active_cpus;
} proc_mem_t;

typedef enum {
    PRIO_IDLE = 0,
    PRIO_LOW  = 5,
    PRIO_USER = 10,
    PRIO_HIGH = 15,
    PRIO_GUI  = 20,
    PRIO_SUPER = 30,
} task_prio_t;

typedef struct file_desc {
    vfs_node_t* node;

    uint32_t offset;
    uint32_t flags;
    
    uint32_t refs;
    
    spinlock_t lock;
    
    rcu_head_t rcu;
} file_desc_t;

typedef enum {
    TASK_BLOCK_NONE = 0,
    TASK_BLOCK_SEM = 1,
    TASK_BLOCK_FUTEX = 2,
} task_block_kind_t;

typedef struct fd_table {
    uint32_t refs;

    rwspinlock_t lock;
    rcu_ptr_t* fds;
    
    uint32_t max_fds;
    int fd_next;
    
    rcu_head_t rcu;
} fd_table_t;

typedef struct task {
    /* cacheline 1 */

    uint32_t pid __cacheline_aligned;
    
    uint32_t parent_pid;

    task_prio_t priority;
    
    uint32_t kstack_size;
    void* kstack;

    void* tls_base;
    
    void (*entry)(void*);
    void* arg;
    
    char name[32];

    /* cacheline 2 */

    uint64_t vruntime __cacheline_aligned;

    uint64_t exec_start;
    
    uint32_t ticks_left;
    uint32_t quantum;
    
    uint32_t* esp;
    
    proc_mem_t* mem;
    
    uint8_t* fpu_state;
    uint32_t fpu_state_size;
    
    int assigned_cpu;
    
    uint32_t stack_bottom;
    uint32_t stack_top;

    /* cacheline 3 */

    struct rb_node rb_node __cacheline_aligned;

    int is_queued;

    /* cacheline 4 */

    spinlock_t state_lock __cacheline_aligned;

    volatile task_state_t state;
    
    volatile uint32_t refs;
    volatile uint32_t in_transit;
    
    volatile uint32_t pending_signals;
    
    int is_running_signal;

    /* cacheline 5 */

    struct rb_node sleep_rb __cacheline_aligned;

    volatile uint32_t wake_tick;
    volatile int sleep_cpu;
    
    dlist_head_t sem_node;
    void* blocked_on_sem;
    
    task_block_kind_t blocked_kind;
    int is_blocked_on_kbd;
    
    uint32_t wait_for_pid;

    /* cacheline 6 */
    fd_table_t* fd_table __cacheline_aligned;

    uint32_t cwd_inode;
    
    void* terminal;
    vfs_node_t* controlling_tty;
    
    spinlock_t poll_lock;
    dlist_head_t poll_waiters;
    
    uint8_t term_mode;

    /* cacheline 7+ */
    uint32_t start_tick __cacheline_aligned;

    uint32_t sid;
    
    uint32_t pgid;
    dlist_head_t pgrp_node;
    
    dlist_head_t children_list;
    dlist_head_t sibling_node;

    sig_handler_t handlers[NSIG];
    registers_t signal_context;   

    uint32_t last_fault_cr2;
    uint32_t last_fault_eip;
    uint32_t last_fault_err;
    uint8_t last_fault_int;
    uint8_t last_fault_pad[3];

    volatile uint32_t exit_waiters;
    int exit_status;
    semaphore_t exit_sem;

    dlist_head_t zombie_node;
    struct task* zombie_next;

    dlist_head_t all_tasks_node;
    rcu_head_t all_tasks_rcu;

    struct task* hash_next;
    struct task* hash_prev;

} __cacheline_aligned task_t;

void proc_fd_table_init(task_t* t);

file_desc_t* proc_fd_get(task_t* t, int fd);

int proc_fd_alloc(task_t* t, file_desc_t** out_desc);
int proc_fd_add_at(task_t* t, int fd, file_desc_t** out_desc);

int proc_fd_install_at(task_t* t, int fd, file_desc_t* desc);
int proc_fd_remove(task_t* t, int fd, file_desc_t** out_desc);

void proc_fd_table_retain(fd_table_t* ft);
void proc_fd_table_release(fd_table_t* ft);

void file_desc_retain(file_desc_t* d);
void file_desc_release(file_desc_t* d);

void proc_init(void);

uint32_t proc_list_snapshot(yos_proc_info_t* out, uint32_t cap);

task_t* proc_spawn_kthread(const char* name, task_prio_t prio, void (*entry)(void*), void* arg);

task_t* proc_clone_thread(uint32_t entry, uint32_t arg, uint32_t stack_bottom, uint32_t stack_top);
task_t* proc_spawn_elf(const char* filename, int argc, char** argv);

void proc_kill(task_t* t);
void proc_wait(uint32_t pid);

int proc_setsid(task_t* t);
int proc_setpgid(task_t* t, uint32_t pgid);

uint32_t proc_getpgrp(task_t* t);

int proc_signal_pgrp(uint32_t pgid, uint32_t sig);

int proc_pgrp_in_session(uint32_t pgid, uint32_t sid);

int proc_waitpid(uint32_t pid, int* out_status);
void reaper_task_func(void* arg);

task_t* proc_get_list_head();
task_t* proc_task_at(uint32_t idx);

task_t* proc_create_idle(int cpu_index);
task_t* proc_find_by_pid(uint32_t pid);

uint32_t proc_task_count(void);

int proc_task_retain(task_t* t);
void proc_task_put(task_t* t);

void proc_sleep_remove(task_t* t);
void proc_check_sleepers(uint32_t current_tick);

void proc_sleep_add(task_t* t, uint32_t wake_tick);
void proc_usleep(uint32_t us);

void proc_wake(task_t* t);

int proc_change_state(task_t* t, task_state_t new_state);

void proc_invoke_oom_killer(void);

void proc_mem_retain(proc_mem_t* mem);
void proc_mem_release(proc_mem_t* mem);

___inline task_t* proc_current() { 
    cpu_t* cpu = cpu_current();
    
    return cpu->current_task; 
}

#ifdef __cplusplus
}
#endif

#endif
