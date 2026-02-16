// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#ifndef KERNEL_PROC_H
#define KERNEL_PROC_H

#include <arch/i386/idt.h>
#include <fs/vfs.h>
#include <hal/lock.h>
#include <lib/dlist.h>
#include <lib/rbtree.h>
#include <yos/proc.h>

#include <stdint.h>

#define KSTACK_SIZE 32768

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

#define MAP_SHARED  1
#define MAP_PRIVATE 2

#define MAP_STACK   4

typedef void (*sig_handler_t)(int);

typedef struct mmap_area {
    uint32_t vaddr_start;
    uint32_t vaddr_end;
    uint32_t file_offset;
    uint32_t length;
    uint32_t file_size; 
    uint32_t map_flags;
    
    struct vfs_node* file;
    
    struct mmap_area* next;
} mmap_area_t;

typedef struct proc_mem {
    uint32_t* page_dir;
    uint32_t prog_break;
    uint32_t heap_start;
    mmap_area_t* mmap_list;
    uint32_t mmap_top;
    uint32_t mem_pages;

    uint32_t fbmap_pages;
    uint32_t fbmap_user_ptr;
    uint32_t fbmap_size_bytes;
    uint8_t fbmap_is_virtio;
    uint8_t fbmap_pad[3];

    uint32_t leader_pid;
    uint32_t refcount;
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
} file_desc_t;

typedef struct fd_table {
    uint32_t refs;
    spinlock_t lock;
    struct file_desc** fds;
    uint32_t max_fds;
    int fd_next;
} fd_table_t;

typedef struct task {
    uint32_t pid;
    int is_queued;
    task_state_t state;
    char name[32];

    struct task* next;
    struct task* prev;
    
    struct rb_node rb_node;

    struct task* hash_next;
    struct task* hash_prev;
    
    int assigned_cpu;

    uint32_t wait_for_pid;
    uint32_t wake_tick;
    int is_blocked_on_kbd;
    
    uint32_t* esp;
    proc_mem_t* mem;
    
    void (*entry)(void*);
    void* arg;
    
    void* kstack;       
    uint32_t kstack_size;
    uint32_t stack_bottom;
    uint32_t stack_top;
    fd_table_t* fd_table;
    uint32_t cwd_inode;

    void* terminal;

    uint32_t pending_signals;
    sig_handler_t handlers[NSIG];
    registers_t signal_context; 
    int is_running_signal;

    uint32_t last_fault_cr2;
    uint32_t last_fault_eip;
    uint32_t last_fault_err;
    uint8_t last_fault_int;
    uint8_t last_fault_pad[3];

    uint32_t parent_pid;

    task_prio_t priority;
    uint32_t    quantum;
    uint32_t    ticks_left;
    
    uint64_t    vruntime;
    uint64_t    exec_start;

    uint8_t term_mode;

    uint8_t* fpu_state;
    uint32_t fpu_state_size;

    dlist_head_t sem_node;
    void* blocked_on_sem;
    
    dlist_head_t zombie_node;

    struct rb_node sleep_rb;
    spinlock_t poll_lock;
    dlist_head_t poll_waiters;

    volatile uint32_t exit_waiters;
    int exit_status;
    semaphore_t exit_sem; 
  
} task_t;

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
task_t* proc_current(void);

uint32_t proc_list_snapshot(yos_proc_info_t* out, uint32_t cap);

task_t* proc_spawn_kthread(const char* name, task_prio_t prio, void (*entry)(void*), void* arg);
task_t* proc_clone_thread(uint32_t entry, uint32_t arg, uint32_t stack_bottom, uint32_t stack_top);
task_t* proc_spawn_elf(const char* filename, int argc, char** argv);

void proc_kill(task_t* t);
void proc_wait(uint32_t pid);
int proc_waitpid(uint32_t pid, int* out_status);
void reaper_task_func(void* arg);

task_t* proc_get_list_head();
uint32_t proc_task_count(void);
task_t* proc_task_at(uint32_t idx);
task_t* proc_create_idle(int cpu_index);
task_t* proc_find_by_pid(uint32_t pid);
void proc_sleep_remove(task_t* t);
void proc_check_sleepers(uint32_t current_tick);
void proc_sleep_add(task_t* t, uint32_t wake_tick);
void proc_usleep(uint32_t us);
void proc_wake(task_t* t);

#endif
