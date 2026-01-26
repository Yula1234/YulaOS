// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#include <drivers/vga.h>
#include <lib/string.h>
#include <lib/dlist.h>
#include <fs/yulafs.h>

#include <arch/i386/paging.h>
#include <arch/i386/gdt.h>

#include <mm/heap.h>
#include <mm/pmm.h>

#include <hal/lock.h>
#include <hal/io.h>
#include <hal/simd.h>
 #include <drivers/fbdev.h>

#include "sched.h"
#include "proc.h"
#include "poll_waitq.h"
#include "elf.h"
#include "cpu.h"

#define PID_HASH_SIZE 16384
#define PID_HASH_LOCKS_COUNT 256

#define EI_CLASS 4
#define EI_DATA 5
#define EI_VERSION 6
#define ELFCLASS32 1
#define ELFDATA2LSB 1
#define EV_CURRENT 1
#define ET_EXEC 2
#define EM_386 3
#define PT_LOAD 1
#define USER_ELF_MIN_VADDR 0x08000000u
#define USER_ELF_MAX_VADDR 0xB0000000u

static task_t* tasks_head = 0;
static task_t* tasks_tail = 0;

static uint32_t total_tasks = 0;
static uint32_t next_pid = 1;

static spinlock_t proc_lock;

static dlist_head_t sleeping_list; 
static spinlock_t sleep_lock;

static task_t* pid_hash[PID_HASH_SIZE];
static spinlock_t pid_hash_locks[PID_HASH_LOCKS_COUNT];

static uint8_t* initial_fpu_state = 0;
static uint32_t initial_fpu_state_size = 0;

extern void irq_return(void);

static void pid_hash_insert(task_t* t) {
    uint32_t idx = t->pid % PID_HASH_SIZE;
    uint32_t lock_idx = t->pid % PID_HASH_LOCKS_COUNT;

    uint32_t flags = spinlock_acquire_safe(&pid_hash_locks[lock_idx]);

    t->hash_next = pid_hash[idx];
    t->hash_prev = 0;
    if (pid_hash[idx]) {
        pid_hash[idx]->hash_prev = t;
    }
    pid_hash[idx] = t;

    spinlock_release_safe(&pid_hash_locks[lock_idx], flags);
}

static void pid_hash_remove(task_t* t) {
    uint32_t idx = t->pid % PID_HASH_SIZE;
    uint32_t lock_idx = t->pid % PID_HASH_LOCKS_COUNT;

    uint32_t flags = spinlock_acquire_safe(&pid_hash_locks[lock_idx]);

    if (t->hash_prev) {
        t->hash_prev->hash_next = t->hash_next;
    } else {
        pid_hash[idx] = t->hash_next;
    }
    if (t->hash_next) {
        t->hash_next->hash_prev = t->hash_prev;
    }
    t->hash_next = 0;
    t->hash_prev = 0;

    spinlock_release_safe(&pid_hash_locks[lock_idx], flags);
}

void proc_init(void) {
    tasks_head = 0;
    tasks_tail = 0;
    total_tasks = 0;
    next_pid = 1;
    dlist_init(&sleeping_list);
    spinlock_init(&proc_lock);
    spinlock_init(&sleep_lock);

    for (int i = 0; i < PID_HASH_LOCKS_COUNT; i++) {
        spinlock_init(&pid_hash_locks[i]);
    }

    memset(pid_hash, 0, sizeof(pid_hash));

    initial_fpu_state_size = fpu_state_size();
    initial_fpu_state = (uint8_t*)kmalloc_a(initial_fpu_state_size);
    if (!initial_fpu_state) {
        return;
    }

    __asm__ volatile("fninit");
    fpu_save(initial_fpu_state);
}

task_t* proc_current() { 
    cpu_t* cpu = cpu_current();
    return cpu->current_task; 
}

void proc_fd_table_init(task_t* t) {
    if (!t) return;
    dlist_init(&t->fd_list);
    t->fd_next = 0;
}

static proc_fd_entry_t* proc_fd_find_entry(task_t* t, int fd) {
    if (!t || fd < 0) return 0;
    proc_fd_entry_t* e;
    dlist_for_each_entry(e, &t->fd_list, list) {
        if (e->fd == fd) return e;
    }
    return 0;
}

file_t* proc_fd_get(task_t* t, int fd) {
    proc_fd_entry_t* e = proc_fd_find_entry(t, fd);
    return e ? &e->file : 0;
}

int proc_fd_add_at(task_t* t, int fd, file_t** out_file) {
    if (!t || fd < 0) return -1;
    if (proc_fd_find_entry(t, fd)) return -1;

    proc_fd_entry_t* e = (proc_fd_entry_t*)kmalloc(sizeof(proc_fd_entry_t));
    if (!e) return -1;
    memset(e, 0, sizeof(*e));
    e->fd = fd;

    proc_fd_entry_t* pos;
    dlist_for_each_entry(pos, &t->fd_list, list) {
        if (pos->fd > fd) {
            dlist_add_tail(&e->list, &pos->list);
            if (fd == t->fd_next) t->fd_next = fd + 1;
            if (out_file) *out_file = &e->file;
            return fd;
        }
    }

    dlist_add_tail(&e->list, &t->fd_list);

    if (fd == t->fd_next) t->fd_next = fd + 1;
    if (out_file) *out_file = &e->file;
    return fd;
}

int proc_fd_alloc(task_t* t, file_t** out_file) {
    if (!t) return -1;

    int expected = t->fd_next;
    if (expected < 0) expected = 0;

    proc_fd_entry_t* e;
    dlist_for_each_entry(e, &t->fd_list, list) {
        if (e->fd < expected) continue;
        if (e->fd == expected) {
            expected++;
            continue;
        }
        if (e->fd > expected) {
            break;
        }
    }

    int fd = proc_fd_add_at(t, expected, out_file);
    if (fd >= 0 && fd == t->fd_next) t->fd_next = fd + 1;
    return fd;
}

int proc_fd_remove(task_t* t, int fd, file_t* out_file) {
    if (!t || fd < 0) return -1;

    proc_fd_entry_t* e = proc_fd_find_entry(t, fd);
    if (!e) return -1;

    if (out_file) *out_file = e->file;
    dlist_del(&e->list);
    kfree(e);

    if (fd < t->fd_next) t->fd_next = fd;
    return 0;
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

task_t* proc_find_by_pid(uint32_t pid) {
    uint32_t idx = pid % PID_HASH_SIZE;
    uint32_t lock_idx = pid % PID_HASH_LOCKS_COUNT;
    
    uint32_t flags = spinlock_acquire_safe(&pid_hash_locks[lock_idx]);
    
    task_t* curr = pid_hash[idx];
    while (curr) {
        if (curr->pid == pid) {
            spinlock_release_safe(&pid_hash_locks[lock_idx], flags);
            return curr;
        }
        curr = curr->hash_next;
    }
    
    spinlock_release_safe(&pid_hash_locks[lock_idx], flags);
    return 0;
}

static task_t* alloc_task(void) {
    uint32_t flags = spinlock_acquire_safe(&proc_lock);
    
    task_t* t = (task_t*)kmalloc_a(sizeof(task_t));
    if (!t) {
        spinlock_release_safe(&proc_lock, flags);
        return 0;
    }
    
    memset(t, 0, sizeof(task_t));
    proc_fd_table_init(t);

    spinlock_init(&t->poll_lock);
    dlist_init(&t->poll_waiters);

    if (!initial_fpu_state) {
        spinlock_release_safe(&proc_lock, flags);
        kfree(t);
        return 0;
    }

    t->fpu_state_size = initial_fpu_state_size;
    t->fpu_state = (uint8_t*)kmalloc_a(t->fpu_state_size);
    if (!t->fpu_state) {
        spinlock_release_safe(&proc_lock, flags);
        kfree(t);
        return 0;
    }

    memcpy(t->fpu_state, initial_fpu_state, t->fpu_state_size);
    
    sem_init(&t->exit_sem, 0); 

    t->blocked_on_sem = 0;
    t->is_queued = 0;
    t->pid = next_pid++;
    t->state = TASK_RUNNABLE;
    t->cwd_inode = 1;
    t->term_mode = 0;
    t->assigned_cpu = -1;
    t->vruntime = 0;
    t->exec_start = 0;

    pid_hash_insert(t);
    
    list_append(t);
    
    spinlock_release_safe(&proc_lock, flags);
    return t;
}

void proc_free_resources(task_t* t) {
    if (!t) return;

    poll_task_cleanup(t);

    proc_fd_entry_t* e;
    proc_fd_entry_t* n;
    dlist_for_each_entry_safe(e, n, &t->fd_list, list) {
        file_t f = e->file;
        dlist_del(&e->list);
        kfree(e);

        if (f.used) {
            vfs_node_t* node = f.node;
            if (node) {
                if (__sync_sub_and_fetch(&node->refs, 1) == 0) {
                    if (node->ops && node->ops->close) {
                        node->ops->close(node);
                    } else {
                        kfree(node);
                    }
                }
            }
        }
    }

    mmap_area_t* m = t->mmap_list;
    while (m) {
        mmap_area_t* next = m->next;
        if (m->file) {
            if (__sync_sub_and_fetch(&m->file->refs, 1) == 0) {
                if (m->file->ops && m->file->ops->close) {
                    m->file->ops->close(m->file);
                } else {
                    kfree(m->file);
                }
            }
        }
        kfree(m);
        m = next;
    }
    t->mmap_list = 0;

    proc_sleep_remove(t);
    
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

    if (t->fpu_state) {
        kfree(t->fpu_state);
        t->fpu_state = 0;
        t->fpu_state_size = 0;
    }
    
    pid_hash_remove(t);

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

    fb_release_by_pid(pid_to_clean);

    if (t->fbmap_pages > 0 && t->page_dir) {
        const uint32_t user_vaddr_start = 0xB1000000u;
        for (uint32_t i = 0; i < t->fbmap_pages; i++) {
            uint32_t v = user_vaddr_start + i * 4096u;
            paging_map(t->page_dir, v, 0, 0);
        }
        if (t->mem_pages >= t->fbmap_pages) t->mem_pages -= t->fbmap_pages;
        else t->mem_pages = 0;
        t->fbmap_pages = 0;
    }

    uint32_t waited_pid = t->wait_for_pid;
    if (waited_pid) {
        task_t* waited = proc_find_by_pid(waited_pid);
        if (waited) {
            uint32_t old = __sync_fetch_and_sub(&waited->exit_waiters, 1);
            if (old == 0) {
                __sync_fetch_and_add(&waited->exit_waiters, 1);
            }
        }
        t->wait_for_pid = 0;
    }

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

    sem_remove_task(t);

    poll_task_cleanup(t);

    proc_sleep_remove(t);

    sched_remove(t);

    t->state = TASK_ZOMBIE;

    sem_signal(&t->exit_sem);
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
    t->mem_pages = 0;
    t->priority = prio;
    
    t->kstack_size = KSTACK_SIZE;
    t->kstack = kmalloc_a(t->kstack_size);
    if (!t->kstack) {
        proc_free_resources(t);
        return 0;
    }
    memset(t->kstack, 0, t->kstack_size);
    
    uint32_t stack_top = (uint32_t)t->kstack + t->kstack_size;
    
    stack_top &= ~0xF; 
    
    uint32_t* sp = (uint32_t*)stack_top;
    *--sp = 0;
    *--sp = 0;
    *--sp = 0;
    
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

    uint32_t aligned_file_size = file_size;
    if (aligned_file_size > 0xFFFFFFFFu - diff) aligned_file_size = 0xFFFFFFFFu;
    else aligned_file_size += diff;
    if (aligned_file_size > aligned_size) aligned_file_size = aligned_size;

    area->vaddr_start = aligned_vaddr;
    area->vaddr_end   = aligned_vaddr + aligned_size;
    area->file_offset = aligned_offset;
    area->length      = size;
    area->file_size   = aligned_file_size;
    area->map_flags   = MAP_PRIVATE;
    area->file        = node;
    
    __sync_fetch_and_add(&node->refs, 1);

    area->next = t->mmap_list;
    t->mmap_list = area;
}

task_t* proc_spawn_elf(const char* filename, int argc, char** argv) {
    vfs_node_t* exec_node = vfs_create_node_from_path(filename);
    if (!exec_node) return 0;

    Elf32_Ehdr header;
    Elf32_Phdr* phdrs = 0;
    uint32_t max_vaddr = 0;

    if (exec_node->ops->read(exec_node, 0, sizeof(Elf32_Ehdr), &header) < (int)sizeof(Elf32_Ehdr)) {
        kfree(exec_node);
        return 0;
    }
    
    if (header.e_ident[0] != 0x7F || header.e_ident[1] != 'E' || header.e_ident[2] != 'L' || header.e_ident[3] != 'F') {
        kfree(exec_node);
        return 0;
    }
    if (header.e_ident[EI_CLASS] != ELFCLASS32 || header.e_ident[EI_DATA] != ELFDATA2LSB || header.e_ident[EI_VERSION] != EV_CURRENT) {
        kfree(exec_node);
        return 0;
    }
    if (header.e_type != ET_EXEC || header.e_machine != EM_386 || header.e_version != EV_CURRENT) {
        kfree(exec_node);
        return 0;
    }
    if (header.e_ehsize != sizeof(Elf32_Ehdr) || header.e_phentsize != sizeof(Elf32_Phdr)) {
        kfree(exec_node);
        return 0;
    }
    if (header.e_phnum == 0 || header.e_phnum > 64) {
        kfree(exec_node);
        return 0;
    }
    {
        uint64_t ph_end = (uint64_t)header.e_phoff + (uint64_t)header.e_phnum * (uint64_t)sizeof(Elf32_Phdr);
        if (header.e_phoff == 0 || ph_end > (uint64_t)exec_node->size) {
            kfree(exec_node);
            return 0;
        }
    }

    size_t phdr_bytes = (size_t)header.e_phnum * sizeof(Elf32_Phdr);
    phdrs = (Elf32_Phdr*)kmalloc(phdr_bytes);
    if (!phdrs) {
        kfree(exec_node);
        return 0;
    }
    if (exec_node->ops->read(exec_node, header.e_phoff, (uint32_t)phdr_bytes, phdrs) < (int)phdr_bytes) {
        kfree(phdrs);
        kfree(exec_node);
        return 0;
    }

    int have_load = 0;
    int entry_ok = 0;
    for (int i = 0; i < header.e_phnum; i++) {
        if (phdrs[i].p_type != PT_LOAD) continue;

        uint32_t start_v = phdrs[i].p_vaddr;
        uint32_t mem_sz  = phdrs[i].p_memsz;
        uint32_t file_off= phdrs[i].p_offset;
        uint32_t file_sz = phdrs[i].p_filesz;

        if (mem_sz < file_sz) { kfree(phdrs); kfree(exec_node); return 0; }

        uint32_t end_v = start_v + mem_sz;
        if (end_v < start_v) { kfree(phdrs); kfree(exec_node); return 0; }
        if (start_v < USER_ELF_MIN_VADDR || end_v > USER_ELF_MAX_VADDR) { kfree(phdrs); kfree(exec_node); return 0; }

        uint32_t diff = start_v & 0xFFFu;
        if (file_off < diff) { kfree(phdrs); kfree(exec_node); return 0; }

        uint64_t file_end = (uint64_t)file_off + (uint64_t)file_sz;
        if (file_end > (uint64_t)exec_node->size) { kfree(phdrs); kfree(exec_node); return 0; }

        have_load = 1;
        if (end_v > max_vaddr) max_vaddr = end_v;
        if (header.e_entry >= start_v && header.e_entry < end_v) entry_ok = 1;
    }
    if (!have_load || !entry_ok || max_vaddr == 0) {
        kfree(phdrs);
        kfree(exec_node);
        return 0;
    }

    char** k_argv = (char**)kmalloc((argc + 1) * sizeof(char*));
    if (!k_argv) {
        kfree(phdrs);
        kfree(exec_node);
        return 0;
    }
    
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
        kfree(phdrs);
        kfree(exec_node);
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
        proc_fd_entry_t* pe;
        dlist_for_each_entry(pe, &parent->fd_list, list) {
            file_t* nf = 0;
            if (proc_fd_add_at(t, pe->fd, &nf) < 0) continue;
            if (nf) {
                *nf = pe->file;
                if (nf->node) {
                    __sync_fetch_and_add(&nf->node->refs, 1);
                }
            }
        }
    } else {
        file_t* f0 = 0;
        file_t* f1 = 0;
        file_t* f2 = 0;
        if (proc_fd_add_at(t, 0, &f0) >= 0 && f0) {
            vfs_node_t* dev = devfs_fetch("kbd");
            if (dev) {
                vfs_node_t* node = (vfs_node_t*)kmalloc(sizeof(vfs_node_t));
                if (node) {
                    memcpy(node, dev, sizeof(vfs_node_t));
                    node->refs = 1;
                    f0->node = node;
                }
            }
            f0->offset = 0;
            f0->used = (f0->node != 0);
        }
        if (proc_fd_add_at(t, 1, &f1) >= 0 && f1) {
            vfs_node_t* dev = devfs_fetch("console");
            if (dev) {
                vfs_node_t* node = (vfs_node_t*)kmalloc(sizeof(vfs_node_t));
                if (node) {
                    memcpy(node, dev, sizeof(vfs_node_t));
                    node->refs = 1;
                    f1->node = node;
                }
            }
            f1->offset = 0;
            f1->used = (f1->node != 0);
        }
        if (proc_fd_add_at(t, 2, &f2) >= 0 && f2 && f1 && f1->used) {
            *f2 = *f1;
            if (f2->node) {
                __sync_fetch_and_add(&f2->node->refs, 1);
            }
        } else if (f2) {
            f2->used = 0;
            f2->node = 0;
        }
    }
    
    t->priority = PRIO_USER;
    strlcpy(t->name, filename, 32);

    t->kstack_size = KSTACK_SIZE;
    t->kstack = kmalloc_a(t->kstack_size);
    if (!t->kstack) {
        kfree(exec_node);
        for(int i=0; i<argc; i++) kfree(k_argv[i]);
        kfree(k_argv);
        kfree(phdrs);
        proc_free_resources(t);
        return 0;
    }
    memset(t->kstack, 0, t->kstack_size);

    t->page_dir = paging_clone_directory();
    
    uint32_t* my_pd = paging_get_dir();

    t->mmap_list = 0;
    t->mmap_top = 0x80001000;

    for (int i = 0; i < header.e_phnum; i++) {
        if (phdrs[i].p_type == 1) {
            uint32_t start_v = phdrs[i].p_vaddr;
            uint32_t mem_sz  = phdrs[i].p_memsz;
            uint32_t file_off= phdrs[i].p_offset;
            uint32_t file_sz = phdrs[i].p_filesz;
            
            proc_add_mmap_region(t, exec_node, start_v, mem_sz, file_sz, file_off);
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
    task_t* target = 0;

    task_t* waiter = proc_current();

    uint32_t flags = spinlock_acquire_safe(&proc_lock);
    task_t* curr = tasks_head;
    while (curr) {
        if (curr->pid == pid) {
            target = curr;
            __sync_fetch_and_add(&target->exit_waiters, 1);
            if (waiter) waiter->wait_for_pid = pid;
            break;
        }
        curr = curr->next;
    }
    spinlock_release_safe(&proc_lock, flags);

    if (!target) return;

    sem_wait(&target->exit_sem);

    if (waiter) waiter->wait_for_pid = 0;
    __sync_fetch_and_sub(&target->exit_waiters, 1);
}

 int proc_waitpid(uint32_t pid, int* out_status) {
     task_t* target = 0;

     task_t* waiter = proc_current();

     uint32_t flags = spinlock_acquire_safe(&proc_lock);
     task_t* curr = tasks_head;
     while (curr) {
         if (curr->pid == pid) {
             target = curr;
             __sync_fetch_and_add(&target->exit_waiters, 1);
             if (waiter) waiter->wait_for_pid = pid;
             break;
         }
         curr = curr->next;
     }
     spinlock_release_safe(&proc_lock, flags);

     if (!target) return -1;

     sem_wait(&target->exit_sem);

     if (waiter) waiter->wait_for_pid = 0;

     if (out_status) {
         *out_status = target->exit_status;
     }

     __sync_fetch_and_sub(&target->exit_waiters, 1);
     return 0;
 }


void reaper_task_func(void* arg) {
    (void)arg;
    while(1) {
        uint32_t flags = spinlock_acquire_safe(&proc_lock);
        task_t* curr = tasks_head;
        
        while (curr) {
            if (curr->state == TASK_ZOMBIE) {
                int still_running = 0;
                for (int i = 0; i < MAX_CPUS; i++) {
                    if (cpus[i].current_task == curr) {
                        still_running = 1;
                        break;
                    }
                }
                
                int has_waiters = !dlist_empty(&curr->exit_sem.wait_list);

                if (still_running || has_waiters || curr->exit_waiters > 0) {
                    curr = curr->next;
                    continue;
                }

                curr->state = TASK_UNUSED; 
                
                spinlock_release_safe(&proc_lock, flags);
                
                sched_remove(curr);
                
                proc_free_resources(curr);
                
                flags = spinlock_acquire_safe(&proc_lock);
                curr = tasks_head;
                continue; 
            }
            curr = curr->next;
        }
        
        spinlock_release_safe(&proc_lock, flags);

        __asm__ volatile("int $0x80" : : "a"(7), "b"(50)); // sleep(50ms)
    }
}

task_t* proc_create_idle(int cpu_index) {
    task_t* t = alloc_task();
    if (!t) return 0;
    
    uint32_t flags = spinlock_acquire_safe(&proc_lock);
    list_remove(t);
    pid_hash_remove(t);
    spinlock_release_safe(&proc_lock, flags);

    strlcpy(t->name, "idle", 32);
    t->state = TASK_RUNNING;
    t->pid = 0;             
    t->assigned_cpu = cpu_index;
    t->mem_pages = 0;
    t->page_dir = kernel_page_directory;

    t->kstack_size = KSTACK_SIZE;
    t->priority = PRIO_IDLE;
    
    t->kstack = kmalloc_a(t->kstack_size);
    memset(t->kstack, 0, t->kstack_size);
    
    uint32_t stack_top = (uint32_t)t->kstack + t->kstack_size;
    stack_top &= ~0xF;
    uint32_t* sp = (uint32_t*)stack_top;
    
    extern void idle_task_func(void*);
    
    *--sp = 0;
    *--sp = 0; // Fake Return Address
    *--sp = (uint32_t)idle_task_func;
    *--sp = 0; // EBP
    *--sp = 0; // EBX
    *--sp = 0; // ESI
    *--sp = 0; // EDI
    
    t->esp = sp;
    
    return t;
}

void proc_sleep_add(task_t* t, uint32_t wake_tick) {
    uint32_t flags = spinlock_acquire_safe(&sleep_lock);
    
    if (t->sleep_node.next != 0 && t->sleep_node.prev != 0) {
        dlist_del(&t->sleep_node);
        t->sleep_node.next = 0;
        t->sleep_node.prev = 0;
    }

    t->wake_tick = wake_tick;
    t->state = TASK_WAITING;
    
    task_t *curr;
    int inserted = 0;

    dlist_for_each_entry(curr, &sleeping_list, sleep_node) {
        if (curr->wake_tick > wake_tick) {
            dlist_add_tail(&t->sleep_node, &curr->sleep_node);
            inserted = 1;
            break;
        }
    }

    if (!inserted) {
        dlist_add_tail(&t->sleep_node, &sleeping_list);
    }
    
    spinlock_release_safe(&sleep_lock, flags);
    sched_yield();
}

void proc_check_sleepers(uint32_t current_tick) {
    if (dlist_empty(&sleeping_list)) return;

    task_t *first = container_of(sleeping_list.next, task_t, sleep_node);

    if (first->wake_tick > current_tick) return;

    if (spinlock_try_acquire(&sleep_lock)) {
        task_t *pos, *n;
        dlist_for_each_entry_safe(pos, n, &sleeping_list, sleep_node) {
            if (pos->wake_tick <= current_tick) {
                dlist_del(&pos->sleep_node);
                pos->sleep_node.next = 0;
                pos->sleep_node.prev = 0;
                
                pos->wake_tick = 0;
                pos->state = TASK_RUNNABLE;
                sched_add(pos);
            } else {
                break;
            }
        }
        spinlock_release(&sleep_lock);
    }
}

void proc_sleep_remove(task_t* t) {
    uint32_t flags = spinlock_acquire_safe(&sleep_lock);
    
    if (t->sleep_node.next != 0 && t->sleep_node.prev != 0) {
        dlist_del(&t->sleep_node);
        t->sleep_node.next = 0;
        t->sleep_node.prev = 0;
    }
    
    spinlock_release_safe(&sleep_lock, flags);
}

void proc_wake(task_t* t) {
    if (!t) return;
    if (t->state == TASK_ZOMBIE || t->state == TASK_UNUSED) return;

    proc_sleep_remove(t);
    t->wake_tick = 0;

    if (t->blocked_on_sem) return;

    if (t->state == TASK_WAITING) {
        t->state = TASK_RUNNABLE;
        sched_add(t);
    }
}