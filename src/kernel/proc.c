#include <drivers/vga.h>
#include <lib/string.h>
#include <fs/yulafs.h>

#include <arch/i386/paging.h>
#include <arch/i386/gdt.h>

#include <mm/heap.h>
#include <mm/pmm.h>

#include <hal/lock.h>
#include <hal/io.h>


#include "sched.h"
#include "proc.h"
#include "elf.h"
#include "cpu.h"

#define KSTACK_SIZE 16384  

static task_t* tasks_head = 0;
static task_t* tasks_tail = 0;
static uint32_t total_tasks = 0;
static uint32_t next_pid = 1;
static spinlock_t proc_lock;

extern void irq_return(void);

void proc_init(void) {
    tasks_head = 0;
    tasks_tail = 0;
    total_tasks = 0;
    next_pid = 1;
    spinlock_init(&proc_lock);
}

task_t* proc_current() { 
    cpu_t* cpu = cpu_current();
    return cpu->current_task; 
}

static void list_append(task_t* t) {
    t->next = 0;
    t->prev = 0;
    
    if (!tasks_head) {
        tasks_head = t;
        tasks_tail = t;
    } else {
        tasks_tail->next = t;
        t->prev = tasks_tail;
        tasks_tail = t;
    }
    total_tasks++;
}

static void list_remove(task_t* t) {
    if (t->prev) t->prev->next = t->next;
    else tasks_head = t->next;

    if (t->next) t->next->prev = t->prev;
    else tasks_tail = t->prev;

    t->next = 0;
    t->prev = 0;
    
    if (total_tasks > 0) total_tasks--;
}

static task_t* alloc_task(void) {
    uint32_t flags = spinlock_acquire_safe(&proc_lock);
    
    task_t* t = (task_t*)kmalloc(sizeof(task_t));
    if (!t) {
        spinlock_release_safe(&proc_lock, flags);
        return 0;
    }
    
    memset(t, 0, sizeof(task_t));
    t->pid = next_pid++;
    t->state = TASK_RUNNABLE;
    t->cwd_inode = 1;
    t->term_mode = 0;
    
    list_append(t);
    
    spinlock_release_safe(&proc_lock, flags);
    return t;
}

void proc_free_resources(task_t* t) {
    if (!t) return;
    
    for (int i = 0; i < MAX_PROCESS_FDS; i++) {
        if (t->fds[i].used) {
            file_t* f = &t->fds[i];
            vfs_node_t* node = f->node;
            
            if (node) {
                if (node->refs > 0) node->refs--;
                
                if (node->refs == 0) {
                    if (node->ops && node->ops->close) {
                        node->ops->close(node);
                    } else {
                        kfree(node);
                    }
                }
            }
            f->used = 0; 
            f->node = 0;
        }
    }

    mmap_area_t* m = t->mmap_list;
    while (m) {
        mmap_area_t* next = m->next;
        if (m->file && m->file->refs > 0) m->file->refs--; 
        kfree(m);
        m = next;
    }
    t->mmap_list = 0;
    
    if (t->page_dir && t->page_dir != kernel_page_directory) {
        for (int i = 0; i < 1024; i++) {
            uint32_t pde = t->page_dir[i];
            
            if (!(pde & 1)) continue;
            
            if (kernel_page_directory[i] == pde) {
                continue; 
            }
            
            if (pde & 4) { 
                uint32_t* pt = (uint32_t*)(pde & ~0xFFF);
                
                for (int j = 0; j < 1024; j++) {
                    uint32_t pte = pt[j];
                    
                    if ((pte & 1)) {
                        if (pte & 0x200) {
                            pt[j] = 0;
                        } 
                        else if (pte & 4) {
                            void* physical_page = (void*)(pte & ~0xFFF);
                            pmm_free_block(physical_page);
                        }
                    }
                }
                pmm_free_block(pt);
            }
        }
        pmm_free_block(t->page_dir);
        t->page_dir = 0;
    }
    
    if (t->kstack) { 
        kfree(t->kstack); 
        t->kstack = 0; 
    }
    
    t->mem_pages = 0;
    t->pid = 0;
    memset(t->name, 0, sizeof(t->name));
    
    uint32_t flags = spinlock_acquire_safe(&proc_lock);
    list_remove(t);
    kfree(t);
    spinlock_release_safe(&proc_lock, flags);
}

void proc_kill(task_t* t) {
    if (!t) return;
    
    uint32_t pid_to_clean = t->pid;

    while (1) {
        int found_child = 0;
        uint32_t flags = spinlock_acquire_safe(&proc_lock);
        task_t* child = tasks_head;
        while (child) {
            if (child->parent_pid == pid_to_clean && child->state != TASK_ZOMBIE && child->state != TASK_UNUSED) {
                found_child = 1;
                spinlock_release_safe(&proc_lock, flags);
                proc_kill(child);
                break; 
            }
            child = child->next;
        }
        if (!found_child) {
            spinlock_release_safe(&proc_lock, flags);
            break;
        }
    }

    sched_remove(t);
    
    extern void window_close_all_by_pid(int pid);
    window_close_all_by_pid(pid_to_clean);

    t->state = TASK_ZOMBIE;
    
    proc_wake_up_waiters(pid_to_clean);
}

static void kthread_trampoline(void) {
    task_t* t = proc_current();
    __asm__ volatile("sti");
    t->entry(t->arg);       
    
    t->state = TASK_ZOMBIE;
    sched_yield();        
    for (;;) cpu_hlt();   
}

task_t* proc_get_list_head() { return tasks_head; }
uint32_t proc_task_count(void) { return total_tasks; }

task_t* proc_task_at(uint32_t idx) {
    uint32_t flags = spinlock_acquire_safe(&proc_lock);
    task_t* curr = tasks_head;
    uint32_t i = 0;
    while (curr) {
        if (i == idx) {
             spinlock_release_safe(&proc_lock, flags);
             return curr;
        }
        curr = curr->next;
        i++;
    }
    spinlock_release_safe(&proc_lock, flags);
    return 0;
}

task_t* proc_spawn_kthread(const char* name, task_prio_t prio, void (*entry)(void*), void* arg) {
    task_t* t = alloc_task();
    if (!t) return 0;
    
    strlcpy(t->name, name ? name : "task", sizeof(t->name));
    t->entry = entry; 
    t->arg = arg;
    t->page_dir = 0;
    t->mem_pages = 4;
    t->priority = prio;
    
    t->kstack_size = KSTACK_SIZE;
    t->kstack = kmalloc_a(t->kstack_size);
    if (!t->kstack) {
        proc_free_resources(t);
        return 0;
    }
    memset(t->kstack, 0, t->kstack_size);
    
    uint32_t* sp = (uint32_t*)((uint32_t)t->kstack + t->kstack_size);
    *--sp = (uint32_t)kthread_trampoline;
    *--sp = 0; // EBP
    *--sp = 0; // EBX
    *--sp = 0; // ESI
    *--sp = 0; // EDI
    t->esp = sp;
    
    sched_add(t);
    return t;
}

static void proc_add_mmap_region(task_t* t, vfs_node_t* node, uint32_t vaddr, uint32_t size, uint32_t file_size, uint32_t offset) {
    mmap_area_t* area = kmalloc(sizeof(mmap_area_t));
    if (!area) return;

    uint32_t aligned_vaddr = vaddr & ~0xFFF;
    
    uint32_t diff = vaddr - aligned_vaddr;
    
    uint32_t aligned_offset = offset - diff;
    
    uint32_t aligned_size = (size + diff + 4095) & ~4095;

    area->vaddr_start = aligned_vaddr;
    area->vaddr_end   = aligned_vaddr + aligned_size;
    area->file_offset = aligned_offset;
    area->length      = size;
    area->file_size   = file_size;
    area->file        = node;
    
    node->refs++;

    area->next = t->mmap_list;
    t->mmap_list = area;
}

task_t* proc_spawn_elf(const char* filename, int argc, char** argv) {
    vfs_node_t* exec_node = vfs_create_node_from_path(filename);
    if (!exec_node) return 0;

    Elf32_Ehdr header;

    if (exec_node->ops->read(exec_node, 0, sizeof(Elf32_Ehdr), &header) < (int)sizeof(Elf32_Ehdr)) {
        kfree(exec_node);
        return 0;
    }
    
    if (header.e_ident[0] != 0x7F || header.e_ident[1] != 'E') {
        kfree(exec_node);
        return 0;
    }

    char** k_argv = (char**)kmalloc((argc + 1) * sizeof(char*));
    if (!k_argv) return 0;
    
    for (int i = 0; i < argc; i++) {
        int len = strlen(argv[i]) + 1;
        k_argv[i] = (char*)kmalloc(len);
        memcpy(k_argv[i], argv[i], len);
    }
    k_argv[argc] = 0;

    task_t* t = alloc_task();
    if (!t) {
        for(int i=0; i<argc; i++) kfree(k_argv[i]);
        kfree(k_argv);
        return 0;
    }

    if (proc_current()) {
        t->cwd_inode = proc_current()->cwd_inode;
        t->parent_pid = proc_current()->pid;
        t->terminal = proc_current()->terminal;
        t->term_mode = proc_current()->term_mode; 
    } else {
        t->cwd_inode = 1;
        t->parent_pid = 0;
    }

     if (proc_current()) {
        task_t* parent = proc_current();
        for (int i = 0; i < MAX_PROCESS_FDS; i++) {
            if (parent->fds[i].used) {
                t->fds[i] = parent->fds[i];
                if (t->fds[i].node) {
                    t->fds[i].node->refs++;
                }
            }
        }
    } else {
        t->fds[0].node = devfs_fetch("kbd");     t->fds[0].used = 1;
        t->fds[1].node = devfs_fetch("console"); t->fds[1].used = 1;
        t->fds[2] = t->fds[1]; 
    }
    
    t->priority = PRIO_USER;
    strlcpy(t->name, filename, 32);

    t->kstack_size = KSTACK_SIZE;
    t->kstack = kmalloc_a(t->kstack_size);
    if (!t->kstack) {
        kfree(exec_node);
        for(int i=0; i<argc; i++) kfree(k_argv[i]);
        kfree(k_argv);
        proc_free_resources(t);
        return 0;
    }
    memset(t->kstack, 0, t->kstack_size);

    t->page_dir = paging_clone_directory();
    
    uint32_t* my_pd = paging_get_dir();

    t->mmap_list = 0;
    t->mmap_top = 0x80001000;

    Elf32_Phdr* phdrs = (Elf32_Phdr*)kmalloc(header.e_phnum * sizeof(Elf32_Phdr));
    exec_node->ops->read(exec_node, header.e_phoff, header.e_phnum * sizeof(Elf32_Phdr), phdrs);

    uint32_t max_vaddr = 0;

    for (int i = 0; i < header.e_phnum; i++) {
        if (phdrs[i].p_type == 1) {
            uint32_t start_v = phdrs[i].p_vaddr;
            uint32_t mem_sz  = phdrs[i].p_memsz;
            uint32_t file_off= phdrs[i].p_offset;
            uint32_t file_sz = phdrs[i].p_filesz;
            
            proc_add_mmap_region(t, exec_node, start_v, mem_sz, file_sz, file_off);
            
            uint32_t end_v = start_v + mem_sz;
            if (end_v > max_vaddr) max_vaddr = end_v;
        }
    }
    
    kfree(phdrs);

    uint32_t start_pde_idx = 0x08000000 >> 22;
    uint32_t end_pde_idx   = (max_vaddr - 1) >> 22;

    for (uint32_t i = start_pde_idx; i <= end_pde_idx; i++) {
        t->page_dir[i] = 0;
    }
    
    t->prog_break = (max_vaddr + 0xFFF) & ~0xFFF;
    t->heap_start = t->prog_break;
    
    if (t->mmap_top < t->prog_break) t->mmap_top = t->prog_break + 0x100000;

    uint32_t stack_size = 4 * 1024 * 1024; 
    uint32_t ustack_top_limit = 0xB0400000;
    uint32_t ustack_bottom = ustack_top_limit - stack_size;

    t->stack_bottom = ustack_bottom;
    t->stack_top = ustack_top_limit;

    for (int i = 1; i <= 4; i++) {
        uint32_t addr = ustack_top_limit - i * 4096;
        void* p = pmm_alloc_block();
        if (p) {
            paging_map(t->page_dir, addr, (uint32_t)p, 7);
            t->mem_pages++;
        }
    }

    __asm__ volatile("cli");
    paging_switch(t->page_dir);

    uint32_t ustack_top = ustack_top_limit; 

    uint32_t arg_ptrs[16];
    int actual_argc = (argc > 16) ? 16 : argc;

    for (int i = actual_argc - 1; i >= 0; i--) {
        size_t len = strlen(k_argv[i]) + 1;
        ustack_top -= len;
        memcpy((void*)ustack_top, k_argv[i], len);
        arg_ptrs[i] = ustack_top;
    }

    ustack_top &= ~0xF;
    uint32_t* us = (uint32_t*)ustack_top;
    
    *--us = 0;
    
    for (int i = actual_argc - 1; i >= 0; i--) *--us = arg_ptrs[i];
    
    uint32_t argv_ptr = (uint32_t)us;
    
    *--us = argv_ptr; 
    *--us = (uint32_t)actual_argc;
    *--us = 0;  
    
    uint32_t final_user_esp = (uint32_t)us;

    paging_switch(my_pd);
    __asm__ volatile("sti");

    for(int i=0; i<argc; i++) kfree(k_argv[i]);
    kfree(k_argv);

    uint32_t* ksp = (uint32_t*)((uint32_t)t->kstack + t->kstack_size);
    *--ksp = 0x23;             // SS (User Data)
    *--ksp = final_user_esp;   // ESP
    *--ksp = 0x202;            // EFLAGS (Interrupts enabled)
    *--ksp = 0x1B;             // CS (User Code)
    *--ksp = header.e_entry;     // EIP (Entry point)
    
    *--ksp = (uint32_t)irq_return; 
    
    *--ksp = 0; *--ksp = 0; *--ksp = 0; *--ksp = 0;
    t->esp = ksp;

    sched_add(t);
    return t;
}

void proc_wait(uint32_t pid) {
    task_t* curr = proc_current();
    if (!curr) return;
    
    while (1) {
        int found = 0;
        int is_zombie = 0;
        task_t* target = 0;
        
        uint32_t flags = spinlock_acquire_safe(&proc_lock);
        task_t* t = tasks_head;
        while (t) {
            if (t->pid == pid) { 
                if (t->state == TASK_UNUSED) {
                    found = 0;
                } else {
                    found = 1;
                    target = t;
                    if (t->state == TASK_ZOMBIE) {
                        is_zombie = 1;
                        t->state = TASK_UNUSED;
                    }
                }
                break; 
            }
            t = t->next;
        }
        
        if (found && is_zombie) {
            spinlock_release_safe(&proc_lock, flags);
            proc_free_resources(target); 
            return; 
        }
        
        spinlock_release_safe(&proc_lock, flags);
        
        if (!found) break; 

        if (curr->pending_signals & (1 << 2)) {
            curr->pending_signals &= ~(1 << 2); 
            
            flags = spinlock_acquire_safe(&proc_lock);
            task_t* child = tasks_head;
            task_t* victim = 0;
            while(child) {
                 if (child->pid == pid) { victim = child; break; }
                 child = child->next;
            }
            spinlock_release_safe(&proc_lock, flags);
            
            if (victim) proc_kill(victim);
            continue; 
        }
        
        curr->state = TASK_WAITING;
        curr->wait_for_pid = pid;
        sched_yield();
    }
}


void proc_wake_up_waiters(uint32_t target_pid) {
    uint32_t flags = spinlock_acquire_safe(&proc_lock);
    task_t* t = tasks_head;
    while (t) {
        if (t->state == TASK_WAITING && t->wait_for_pid == target_pid) {
            t->state = TASK_RUNNABLE;
            t->wait_for_pid = 0;
        }
        t = t->next;
    }
    spinlock_release_safe(&proc_lock, flags);
}

void reaper_task_func(void* arg) {
    (void)arg;
    while(1) {
        int found_zombie = 0;
        uint32_t flags = spinlock_acquire_safe(&proc_lock);
        task_t* curr = tasks_head;
        
        while (curr) {
            if (curr->state == TASK_ZOMBIE) {
                curr->state = TASK_UNUSED; 
                
                spinlock_release_safe(&proc_lock, flags);
                
                proc_free_resources(curr);
                
                found_zombie = 1;
                break; 
            }
            curr = curr->next;
        }
        
        if (!found_zombie) {
            spinlock_release_safe(&proc_lock, flags);
        }

        __asm__ volatile("int $0x80" : : "a"(7), "b"(80)); 
    }
}

void proc_wake_up_kbd_waiters() {
    uint32_t flags = spinlock_acquire_safe(&proc_lock);
    task_t* t = tasks_head;
    while (t) {
        if (t->state == TASK_WAITING && t->is_blocked_on_kbd) {
            t->state = TASK_RUNNABLE;
            t->is_blocked_on_kbd = 0;
        }
        t = t->next;
    }
    spinlock_release_safe(&proc_lock, flags);
}