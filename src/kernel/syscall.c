// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#include <arch/i386/paging.h>
#include <lib/string.h>
#include <hal/lock.h>
#include <hal/simd.h>

#include <drivers/vga.h>
#include <drivers/keyboard.h>
#include <drivers/mouse.h>
#include <drivers/fbdev.h>
#include <drivers/virtio_gpu.h>
#include <kernel/input_focus.h>
#include <kernel/rtc.h>
#include <kernel/shm.h>
#include <kernel/ipc_endpoint.h>
#include <kernel/poll_waitq.h>
#include <yos/ioctl.h>
#include <yos/proc.h>

#include <fs/vfs.h>
#include <fs/yulafs.h>
#include <fs/pipe.h>
#include <fs/pty.h>

#include <mm/pmm.h>
#include <mm/heap.h>

#include "clipboard.h"
#include "syscall.h"
#include "sched.h"
#include "proc.h"
#include "cpu.h"

extern volatile uint32_t timer_ticks;

extern uint32_t* paging_get_dir(void); 

extern int smp_fb_present_rect(task_t* owner, const void* src, uint32_t src_stride, int x, int y, int w, int h);

static int check_user_buffer(task_t* task, const void* buf, uint32_t size) {
    if (!buf) return 0;
    if (size == 0) return 1;

    uint32_t start = (uint32_t)buf;
    uint32_t end = start + size;

    if (end < start) return 0; 
    if (start < 0x08000000 || end > 0xC0000000) return 0; 

    if (!task || !task->mem || !task->mem->page_dir) return 0;

    return 1;
}

static void prefault_user_read(const void* p, uint32_t len) {
    if (!p || len == 0) return;

    uintptr_t addr = (uintptr_t)p;
    uintptr_t end = addr + (uintptr_t)len - 1u;
    if (end < addr) return;

    for (uintptr_t cur = addr; cur <= end;) {
        (void)*(volatile const uint8_t*)cur;
        uintptr_t next = (cur & ~(uintptr_t)0xFFFu) + (uintptr_t)0x1000u;
        if (next <= cur) break;
        cur = next;
    }
}

static uint8_t fb_present_fpu_tmp[MAX_CPUS][4096] __attribute__((aligned(64)));

__attribute__((always_inline))
static inline uint32_t irq_save_disable(void) {
    uint32_t flags;
    __asm__ volatile("pushfl; popl %0; cli" : "=r"(flags) : : "memory");
    return flags;
}

__attribute__((always_inline))
static inline void irq_restore(uint32_t flags) {
    __asm__ volatile("pushl %0; popfl" : : "r"(flags) : "memory", "cc");
}

static mmap_area_t* mmap_find_area(task_t* t, uint32_t vaddr);
static int check_user_buffer_writable_present(task_t* task, void* buf, uint32_t size);

static int user_range_mappable(task_t* t, uintptr_t start, uintptr_t end_excl) {
    if (!t || !t->mem || !t->mem->page_dir) return 0;
    if (end_excl <= start) return 0;

    if (start < 0x08000000u || end_excl > 0xC0000000u) return 0;

    uintptr_t cur = start;
    while (cur < end_excl) {
        uint32_t v = (uint32_t)cur;

        if (t->stack_bottom < t->stack_top && v >= t->stack_bottom && v < t->stack_top) {
            uintptr_t lim = (uintptr_t)t->stack_top;
            cur = (end_excl < lim) ? end_excl : lim;
            continue;
        }

        if (t->mem->heap_start < t->mem->prog_break && v >= t->mem->heap_start && v < t->mem->prog_break) {
            uintptr_t lim = (uintptr_t)t->mem->prog_break;
            cur = (end_excl < lim) ? end_excl : lim;
            continue;
        }

        mmap_area_t* m = mmap_find_area(t, v);
        if (!m) return 0;
        if (m->vaddr_start >= m->vaddr_end) return 0;
        if (v < m->vaddr_start || v >= m->vaddr_end) return 0;

        uintptr_t lim = (uintptr_t)m->vaddr_end;
        cur = (end_excl < lim) ? end_excl : lim;
    }
    return 1;
}

static int ensure_user_buffer_writable_mappable(task_t* task, void* buf, uint32_t size) {
    if (!check_user_buffer(task, buf, size)) return 0;
    if (size == 0) return 1;

    uintptr_t start = (uintptr_t)buf;
    uintptr_t end = start + (uintptr_t)size;
    if (end < start) return 0;

    if (!user_range_mappable(task, start, end)) return 0;

    prefault_user_read((const void*)buf, size);
    return check_user_buffer_writable_present(task, buf, size);
}

static int paging_get_present_pte(uint32_t* dir, uint32_t virt, uint32_t* out_pte) {
    if (!dir) return 0;

    uint32_t pd_idx = virt >> 22;
    uint32_t pt_idx = (virt >> 12) & 0x3FF;

    uint32_t pde = dir[pd_idx];
    if (!(pde & 1)) return 0;

    uint32_t* pt = (uint32_t*)(pde & ~0xFFF);
    uint32_t pte = pt[pt_idx];
    if (!(pte & 1)) return 0;

    if (out_pte) *out_pte = pte;
    return 1;
}

static int check_user_buffer_present(task_t* task, const void* buf, uint32_t size) {
    if (!check_user_buffer(task, buf, size)) return 0;
    if (size == 0) return 1;

    if (!task->mem || !task->mem->page_dir) return 0;

    uint32_t start = (uint32_t)buf;
    uint32_t end = start + size;
    if (end < start) return 0;

    uint32_t v = start & ~0xFFFu;
    uint32_t v_end = (end + 0xFFFu) & ~0xFFFu;
    for (; v < v_end; v += 4096u) {
        uint32_t pte;
        if (!paging_get_present_pte(task->mem->page_dir, v, &pte)) return 0;
        if ((pte & 4u) == 0) return 0;
    }
    return 1;
}

static int check_user_buffer_writable_present(task_t* task, void* buf, uint32_t size) {
    if (!check_user_buffer(task, buf, size)) return 0;
    if (size == 0) return 1;

    if (!task->mem || !task->mem->page_dir) return 0;

    uint32_t start = (uint32_t)buf;
    uint32_t end = start + size;
    if (end < start) return 0;

    uint32_t v = start & ~0xFFFu;
    uint32_t v_end = (end + 0xFFFu) & ~0xFFFu;
    for (; v < v_end; v += 4096u) {
        uint32_t pte;
        if (!paging_get_present_pte(task->mem->page_dir, v, &pte)) return 0;
        if ((pte & 4u) == 0) return 0;
        if ((pte & 2u) == 0) return 0;
    }
    return 1;
}

static int copy_user_str_bounded(task_t* task, char* dst, uint32_t dst_size, const char* user_src) {
    if (!task || !dst || dst_size == 0 || !user_src) return -1;

    if (!check_user_buffer(task, user_src, 1)) return -1;

    for (uint32_t i = 0; i < dst_size; i++) {
        const void* p = (const void*)((uint32_t)user_src + i);
        if (!check_user_buffer(task, p, 1)) return -1;
        prefault_user_read(p, 1);
        char c = *(volatile const char*)p;
        dst[i] = c;
        if (c == '\0') return 0;
    }

    return -1;
}

static int yulafs_find_child_name_in_dir(yfs_ino_t dir_ino, yfs_ino_t child_ino, char* out_name, uint32_t out_cap) {
    if (!out_name || out_cap == 0) return -1;
    out_name[0] = '\0';

    if (dir_ino == 0 || child_ino == 0) return -1;

    yfs_dirent_t entries[8];
    uint32_t offset = 0;
    for (;;) {
        int bytes = yulafs_read(dir_ino, (uint8_t*)entries, (yfs_off_t)offset, (uint32_t)sizeof(entries));
        if (bytes <= 0) break;

        int count = bytes / (int)sizeof(yfs_dirent_t);
        if (count <= 0) break;

        for (int i = 0; i < count; i++) {
            if (entries[i].inode != child_ino) continue;
            if (strcmp(entries[i].name, ".") == 0) continue;
            if (strcmp(entries[i].name, "..") == 0) continue;
            strlcpy(out_name, entries[i].name, (size_t)out_cap);
            return 0;
        }

        offset += (uint32_t)bytes;
    }

    return -1;
}

static mmap_area_t* mmap_find_area(task_t* t, uint32_t vaddr) {
    if (!t || !t->mem) return 0;
    mmap_area_t* m = t->mem->mmap_list;
    while (m) {
        if (vaddr >= m->vaddr_start && vaddr < m->vaddr_end) return m;
        m = m->next;
    }
    return 0;
}

static inline uint32_t align_down_4k_u32(uint32_t v) {
    return v & ~0xFFFu;
}

static inline uint32_t align_up_4k_u32(uint32_t v) {
    return (v + 0xFFFu) & ~0xFFFu;
}

static inline int ranges_overlap_u32(uint32_t a_start, uint32_t a_end_excl, uint32_t b_start, uint32_t b_end_excl) {
    return (a_start < b_end_excl) && (b_start < a_end_excl);
}

static mmap_area_t* mmap_find_overlap(task_t* t, uint32_t start, uint32_t end_excl) {
    if (!t || !t->mem) return 0;
    mmap_area_t* m = t->mem->mmap_list;
    while (m) {
        if (ranges_overlap_u32(start, end_excl, m->vaddr_start, m->vaddr_end)) return m;
        m = m->next;
    }
    return 0;
}


#define FUTEX_TABLE_CAP 256u

typedef struct {
    uint32_t in_use;
    uint32_t key;
    semaphore_t sem;
} futex_entry_t;

static spinlock_t futex_table_lock;
static futex_entry_t futex_table[FUTEX_TABLE_CAP];

static semaphore_t* futex_get_sem(uint32_t key) {
    uint32_t flags = spinlock_acquire_safe(&futex_table_lock);

    for (uint32_t i = 0; i < FUTEX_TABLE_CAP; i++) {
        if (!futex_table[i].in_use) continue;
        if (futex_table[i].key == key) {
            spinlock_release_safe(&futex_table_lock, flags);
            return &futex_table[i].sem;
        }
    }

    for (uint32_t i = 0; i < FUTEX_TABLE_CAP; i++) {
        if (futex_table[i].in_use) continue;
        futex_table[i].in_use = 1u;
        futex_table[i].key = key;
        sem_init(&futex_table[i].sem, 0);
        spinlock_release_safe(&futex_table_lock, flags);
        return &futex_table[i].sem;
    }

    for (uint32_t i = 0; i < FUTEX_TABLE_CAP; i++) {
        if (!futex_table[i].in_use) continue;
        semaphore_t* sem = &futex_table[i].sem;
        uint32_t sflags = spinlock_acquire_safe(&sem->lock);
        const int reusable = (sem->count == 0) && dlist_empty(&sem->wait_list);
        spinlock_release_safe(&sem->lock, sflags);
        if (!reusable) continue;

        sem_init(&futex_table[i].sem, 0);
        futex_table[i].key = key;
        spinlock_release_safe(&futex_table_lock, flags);
        return &futex_table[i].sem;
    }

    spinlock_release_safe(&futex_table_lock, flags);
    return 0;
}

static semaphore_t* futex_lookup_sem(uint32_t key) {
    uint32_t flags = spinlock_acquire_safe(&futex_table_lock);
    for (uint32_t i = 0; i < FUTEX_TABLE_CAP; i++) {
        if (!futex_table[i].in_use) continue;
        if (futex_table[i].key == key) {
            spinlock_release_safe(&futex_table_lock, flags);
            return &futex_table[i].sem;
        }
    }
    spinlock_release_safe(&futex_table_lock, flags);
    return 0;
}

static int futex_sem_wait(semaphore_t* sem, volatile const uint32_t* uaddr, uint32_t expected) {
    if (!sem || !uaddr) return -1;

    for (;;) {
        prefault_user_read((const void*)uaddr, 4u);
        uint32_t v = *(volatile const uint32_t*)uaddr;
        if (v != expected) return 0;

        uint32_t flags = spinlock_acquire_safe(&sem->lock);

        v = *(volatile const uint32_t*)uaddr;
        if (v != expected) {
            spinlock_release_safe(&sem->lock, flags);
            return 0;
        }

        if (sem->count > 0) {
            sem->count--;
            task_t* curr = proc_current();
            if (curr) curr->blocked_on_sem = 0;
            spinlock_release_safe(&sem->lock, flags);
            return 0;
        }

        task_t* curr = proc_current();
        if (!curr) {
            spinlock_release_safe(&sem->lock, flags);
            return -1;
        }

        curr->blocked_on_sem = (void*)sem;
        dlist_add_tail(&curr->sem_node, &sem->wait_list);
        curr->state = TASK_WAITING;

        spinlock_release_safe(&sem->lock, flags);
        sched_yield();
    }
}

static int futex_sem_wake(semaphore_t* sem, uint32_t max_wake) {
    if (!sem || max_wake == 0) return 0;

    uint32_t flags = spinlock_acquire_safe(&sem->lock);

    uint32_t woken = 0;
    while (woken < max_wake && !dlist_empty(&sem->wait_list)) {
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
        woken++;
    }

    spinlock_release_safe(&sem->lock, flags);
    return (int)woken;
}


#define MAX_TASKS 32

typedef struct {
    uint32_t type;  // 1=FILE, 2=DIR
    uint32_t size;
} __attribute__((packed)) user_stat_t;

typedef struct {
    uint32_t total_blocks;
    uint32_t free_blocks;
    uint32_t block_size;
} __attribute__((packed)) user_fs_info_t;

typedef struct {
    int32_t x;
    int32_t y;
    int32_t w;
    int32_t h;
} __attribute__((packed)) fb_rect_t;

typedef struct {
    const void* src;
    uint32_t src_stride;
    const fb_rect_t* rects;
    uint32_t rect_count;
} __attribute__((packed)) fb_present_req_t;

typedef struct {
    int32_t fd;
    int16_t events;
    int16_t revents;
} __attribute__((packed)) pollfd_t;

#define POLLIN   0x001
#define POLLOUT  0x004
#define POLLERR  0x008
#define POLLHUP  0x010
#define POLLNVAL 0x020

typedef void (*syscall_fn_t)(registers_t* regs, task_t* curr);

static void syscall_exit(registers_t* regs, task_t* curr) {
    curr->exit_status = (int)regs->ebx;
    proc_kill(curr);
    sched_yield();
}

static void syscall_print(registers_t* regs, task_t* curr) {
    char* s = (char*)regs->ebx;
    if (curr->terminal) {
        term_instance_t* term = (term_instance_t*)curr->terminal;
        spinlock_acquire(&term->lock);
        term_print(term, s);
        spinlock_release(&term->lock);
    } else {
        regs->eax = (uint32_t)-1;
    }
}

static void syscall_getpid(registers_t* regs, task_t* curr) {
    regs->eax = curr->pid;
}

static void syscall_clone(registers_t* regs, task_t* curr) {
    uint32_t entry = regs->ebx;
    uint32_t arg = regs->ecx;
    uint32_t stack_top = regs->edx;
    uint32_t stack_size = regs->esi;

    if (!curr || !curr->mem || !curr->mem->page_dir) {
        regs->eax = (uint32_t)-1;
        return;
    }

    if (entry == 0 || stack_top == 0 || stack_size < 16u) {
        regs->eax = (uint32_t)-1;
        return;
    }

    uint32_t stack_bottom = stack_top - stack_size;
    if (stack_bottom >= stack_top) {
        regs->eax = (uint32_t)-1;
        return;
    }

    if (entry < 0x08000000u || entry >= 0xC0000000u) {
        regs->eax = (uint32_t)-1;
        return;
    }

    if (stack_bottom < 0x08000000u || stack_top > 0xC0000000u) {
        regs->eax = (uint32_t)-1;
        return;
    }

    if (!ensure_user_buffer_writable_mappable(curr, (void*)stack_bottom, stack_size)) {
        regs->eax = (uint32_t)-1;
        return;
    }

    task_t* t = proc_clone_thread(entry, arg, stack_bottom, stack_top);
    regs->eax = t ? t->pid : (uint32_t)-1;
}

static void syscall_open(registers_t* regs, task_t* curr) {
    if (check_user_buffer(curr, (void*)regs->ebx, 1)) {
        regs->eax = (uint32_t)vfs_open((char*)regs->ebx, (int)regs->ecx);
    } else {
        regs->eax = (uint32_t)-1;
    }
}

static void syscall_read(registers_t* regs, task_t* curr) {
    if (ensure_user_buffer_writable_mappable(curr, (void*)regs->ecx, (uint32_t)regs->edx)) {
        int res = vfs_read((int)regs->ebx, (void*)regs->ecx, (uint32_t)regs->edx);
        regs->eax = (uint32_t)res;
    } else {
        regs->eax = (uint32_t)-1;
    }
}

static void syscall_write(registers_t* regs, task_t* curr) {
    if (check_user_buffer(curr, (void*)regs->ecx, (uint32_t)regs->edx)) {
        regs->eax = (uint32_t)vfs_write((int)regs->ebx, (void*)regs->ecx, (uint32_t)regs->edx);
    } else {
        regs->eax = (uint32_t)-1;
    }
}

static void syscall_close(registers_t* regs, task_t* curr) {
    (void)curr;
    regs->eax = (uint32_t)vfs_close((int)regs->ebx);
}

static void syscall_sleep(registers_t* regs, task_t* curr) {
    uint32_t ms = regs->ebx;
    uint32_t target = timer_ticks + (ms * 15u);
    extern void proc_sleep_add(task_t* t, uint32_t tick);
    proc_sleep_add(curr, target);
}

static void syscall_sbrk(registers_t* regs, task_t* curr) {
    int incr = (int)regs->ebx;
    if (!curr->mem || !curr->mem->page_dir) {
        regs->eax = (uint32_t)-1;
        return;
    }

    uint32_t old_brk = curr->mem->prog_break;
    int64_t new_brk64 = (int64_t)(uint64_t)old_brk + (int64_t)incr;
    if (new_brk64 < 0 || new_brk64 >= 0x80000000ll) {
        regs->eax = (uint32_t)-1;
        return;
    }
    uint32_t new_brk = (uint32_t)new_brk64;

    if (new_brk < curr->mem->heap_start) {
        regs->eax = (uint32_t)-1;
        return;
    }

    if (incr > 0) {
        uint32_t chk_start = align_down_4k_u32(old_brk);
        uint32_t chk_end_excl = align_up_4k_u32(new_brk);
        if (chk_end_excl < chk_start) {
            regs->eax = (uint32_t)-1;
            return;
        }

        if (curr->stack_bottom < curr->stack_top) {
            if (ranges_overlap_u32(chk_start, chk_end_excl, curr->stack_bottom, curr->stack_top)) {
                regs->eax = (uint32_t)-1;
                return;
            }
        }

        if (mmap_find_overlap(curr, chk_start, chk_end_excl)) {
            regs->eax = (uint32_t)-1;
            return;
        }
    }

    if (incr < 0) {
        uint32_t start_free = (new_brk + 0xFFFu) & ~0xFFFu;
        uint32_t end_free = (old_brk + 0xFFFu) & ~0xFFFu;

        for (uint32_t v = start_free; v < end_free; v += 4096u) {
            uint32_t pte;
            if (!paging_get_present_pte(curr->mem->page_dir, v, &pte)) continue;
            if ((pte & 4u) == 0) continue;

            paging_map(curr->mem->page_dir, v, 0, 0);

            uint32_t phys = pte & ~0xFFFu;
            if (curr->mem->mem_pages > 0) curr->mem->mem_pages--;
            if (phys && (pte & 0x200u) == 0) {
                pmm_free_block((void*)phys);
            }
        }
    }

    curr->mem->prog_break = new_brk;

    if (incr > 0) {
        uint64_t guard64 = 0x100000ull;
        uint64_t need64 = (uint64_t)new_brk + guard64;
        if (need64 < 0xC0000000ull) {
            uint32_t need = align_up_4k_u32((uint32_t)need64);
            if (curr->mem->mmap_top < need) curr->mem->mmap_top = need;
        }
    }

    regs->eax = old_brk;
}

static void syscall_kill(registers_t* regs, task_t* curr) {
    uint32_t target_pid = regs->ebx;
    task_t* t = proc_find_by_pid(target_pid);
    if (!t) {
        regs->eax = (uint32_t)-1;
        return;
    }

    if (t == curr) {
        proc_kill(t);
        sched_yield();
        regs->eax = 0;
        return;
    }

    int is_running = 0;
    for (int i = 0; i < MAX_CPUS; i++) {
        if (cpus[i].current_task == t) {
            is_running = 1;
            break;
        }
    }

    if (is_running) {
        if (t->mem && t->mem->page_dir) {
            __sync_fetch_and_or(&t->pending_signals, 1u << SIGTERM);
            regs->eax = 0;
        } else {
            regs->eax = (uint32_t)-1;
        }
        return;
    }

    proc_kill(t);
    regs->eax = 0;
}

static void syscall_usleep(registers_t* regs, task_t* curr) {
    (void)curr;
    proc_usleep(regs->ebx);
}

static void syscall_get_mem_stats(registers_t* regs, task_t* curr) {
    uint32_t* u_ptr = (uint32_t*)regs->ebx;
    uint32_t* f_ptr = (uint32_t*)regs->ecx;

    if (check_user_buffer(curr, u_ptr, 4) && check_user_buffer(curr, f_ptr, 4)) {
        *u_ptr = pmm_get_used_blocks() * 4u;
        *f_ptr = pmm_get_free_blocks() * 4u;
        regs->eax = 0;
    } else {
        regs->eax = (uint32_t)-1;
    }
}

static void syscall_mkdir(registers_t* regs, task_t* curr) {
    if (check_user_buffer(curr, (void*)regs->ebx, 1)) {
        regs->eax = (uint32_t)yulafs_mkdir((char*)regs->ebx);
    } else {
        regs->eax = (uint32_t)-1;
    }
}

static void syscall_unlink(registers_t* regs, task_t* curr) {
    if (check_user_buffer(curr, (void*)regs->ebx, 1)) {
        regs->eax = (uint32_t)yulafs_unlink((char*)regs->ebx);
    } else {
        regs->eax = (uint32_t)-1;
    }
}

static void syscall_get_time(registers_t* regs, task_t* curr) {
    if (check_user_buffer(curr, (void*)regs->ebx, 9)) {
        get_time_string((char*)regs->ebx);
        regs->eax = 0;
    } else {
        regs->eax = (uint32_t)-1;
    }
}

static void syscall_reboot(registers_t* regs, task_t* curr) {
    (void)regs;
    (void)curr;
    extern void kbd_reboot(void);
    kbd_reboot();
}

static void syscall_signal(registers_t* regs, task_t* curr) {
    if (regs->ebx < NSIG) {
        curr->handlers[regs->ebx] = (sig_handler_t)regs->ecx;
        regs->eax = 0;
    } else {
        regs->eax = (uint32_t)-1;
    }
}

static void syscall_sigreturn(registers_t* regs, task_t* curr) {
    memcpy(regs, &curr->signal_context, sizeof(registers_t));
    curr->is_running_signal = 0;
}

static void syscall_removed(registers_t* regs, task_t* curr) {
    (void)curr;
    regs->eax = (uint32_t)-1;
}

static void syscall_set_clipboard(registers_t* regs, task_t* curr) {
    char* buf = (char*)regs->ebx;
    int len = (int)regs->ecx;
    if (check_user_buffer(curr, buf, (uint32_t)len)) {
        regs->eax = (uint32_t)clipboard_set(buf, len);
    } else {
        regs->eax = (uint32_t)-1;
    }
}

static void syscall_get_clipboard(registers_t* regs, task_t* curr) {
    char* buf = (char*)regs->ebx;
    int max_len = (int)regs->ecx;
    if (check_user_buffer(curr, buf, (uint32_t)max_len)) {
        regs->eax = (uint32_t)clipboard_get(buf, max_len);
    } else {
        regs->eax = (uint32_t)-1;
    }
}

static void syscall_set_term_mode(registers_t* regs, task_t* curr) {
    int mode = (int)regs->ebx;
    curr->term_mode = (mode == 1) ? 1 : 0;
    regs->eax = 0;
}

static void syscall_set_console_color(registers_t* regs, task_t* curr) {
    uint32_t fg = (uint32_t)regs->ebx;
    uint32_t bg = (uint32_t)regs->ecx;

    if (curr->terminal) {
        term_instance_t* term = (term_instance_t*)curr->terminal;
        spinlock_acquire(&term->lock);
        term->curr_fg = fg;
        term->curr_bg = bg;
        term->def_fg = fg;
        term->def_bg = bg;
        spinlock_release(&term->lock);
    }
    regs->eax = 0;
}

static void syscall_pipe(registers_t* regs, task_t* curr) {
    int* user_fds = (int*)regs->ebx;
    if (!ensure_user_buffer_writable_mappable(curr, user_fds, (uint32_t)sizeof(int) * 2u)) {
        regs->eax = (uint32_t)-1;
        return;
    }

    file_desc_t* fr = 0;
    file_desc_t* fw = 0;

    int fd_r = proc_fd_alloc(curr, &fr);
    if (fd_r < 0 || !fr) {
        regs->eax = (uint32_t)-1;
        return;
    }

    int fd_w = proc_fd_alloc(curr, &fw);
    if (fd_w < 0 || !fw) {
        file_desc_t* tmp = 0;
        (void)proc_fd_remove(curr, fd_r, &tmp);
        file_desc_release(tmp);
        regs->eax = (uint32_t)-1;
        return;
    }

    vfs_node_t* r_node = 0;
    vfs_node_t* w_node = 0;
    if (vfs_create_pipe(&r_node, &w_node) != 0) {
        file_desc_t* tmp = 0;
        (void)proc_fd_remove(curr, fd_r, &tmp);
        file_desc_release(tmp);
        tmp = 0;
        (void)proc_fd_remove(curr, fd_w, &tmp);
        file_desc_release(tmp);
        regs->eax = (uint32_t)-1;
        return;
    }

    fr->node = r_node;
    fr->offset = 0;
    fr->flags = 0;

    fw->node = w_node;
    fw->offset = 0;
    fw->flags = 0;

    user_fds[0] = fd_r;
    user_fds[1] = fd_w;
    regs->eax = 0;
}

static void syscall_dup2(registers_t* regs, task_t* curr) {
    int oldfd = (int)regs->ebx;
    int newfd = (int)regs->ecx;

    if (newfd < 0) {
        regs->eax = (uint32_t)-1;
        return;
    }

    file_desc_t* of = proc_fd_get(curr, oldfd);
    if (!of || !of->node) {
        regs->eax = (uint32_t)-1;
        return;
    }

    if (oldfd == newfd) {
        file_desc_release(of);
        regs->eax = (uint32_t)newfd;
        return;
    }

    if (proc_fd_get(curr, newfd)) {
        (void)vfs_close(newfd);
    }

    if (proc_fd_install_at(curr, newfd, of) < 0) {
        file_desc_release(of);
        regs->eax = (uint32_t)-1;
        return;
    }

    file_desc_release(of);
    regs->eax = (uint32_t)newfd;
}

static void syscall_mmap(registers_t* regs, task_t* curr) {
    int fd = (int)regs->ebx;
    uint32_t size = (uint32_t)regs->ecx;
    int flags = (int)regs->edx;

    uint32_t map_kind = (uint32_t)flags & (MAP_PRIVATE | MAP_SHARED);
    if (!(map_kind == MAP_PRIVATE || map_kind == MAP_SHARED)) {
        regs->eax = 0;
        return;
    }

    if (((uint32_t)flags & ~(MAP_PRIVATE | MAP_SHARED)) != 0) {
        regs->eax = 0;
        return;
    }

    if (size == 0) {
        regs->eax = 0;
        return;
    }

    if (size > 0xFFFFFFFFu - 4095u) {
        regs->eax = 0;
        return;
    }

    file_desc_t* d = proc_fd_get(curr, fd);
    if (fd < 0 || !d || !d->node) {
        regs->eax = 0;
        return;
    }

    if (d->node->flags & (VFS_FLAG_PIPE_READ | VFS_FLAG_PIPE_WRITE)) {
        file_desc_release(d);
        regs->eax = 0;
        return;
    }

    if (d->node->flags & (VFS_FLAG_PTY_MASTER | VFS_FLAG_PTY_SLAVE)) {
        file_desc_release(d);
        regs->eax = 0;
        return;
    }

    if (d->node->flags & VFS_FLAG_SHM) {
        if (map_kind != MAP_SHARED) {
            file_desc_release(d);
            regs->eax = 0;
            return;
        }
        if (!d->node->private_data) {
            file_desc_release(d);
            regs->eax = 0;
            return;
        }
        if (size > d->node->size) {
            file_desc_release(d);
            regs->eax = 0;
            return;
        }
    } else {
        if (map_kind == MAP_SHARED) {
            file_desc_release(d);
            regs->eax = 0;
            return;
        }
        if (!d->node->ops || !d->node->ops->read) {
            file_desc_release(d);
            regs->eax = 0;
            return;
        }
    }

    if (!curr->mem) {
        file_desc_release(d);
        regs->eax = 0;
        return;
    }

    uint32_t size_aligned = (size + 4095u) & ~4095u;
    uint32_t vaddr = align_up_4k_u32(curr->mem->mmap_top);

    if (curr->mem->heap_start < curr->mem->prog_break) {
        uint64_t need64 = (uint64_t)curr->mem->prog_break + 0x100000ull;
        if (need64 < 0xC0000000ull) {
            uint32_t need = align_up_4k_u32((uint32_t)need64);
            if (vaddr < need) vaddr = need;
        }
    }

    int found = 0;
    for (int iter = 0; iter < 256; iter++) {
        uint32_t end_excl = vaddr + size_aligned;
        if (end_excl < vaddr || end_excl > 0xC0000000u) {
            regs->eax = 0;
            return;
        }

        if (curr->stack_bottom < curr->stack_top) {
            if (ranges_overlap_u32(vaddr, end_excl, curr->stack_bottom, curr->stack_top)) {
                regs->eax = 0;
                return;
            }
        }

        if (curr->mem->heap_start < curr->mem->prog_break) {
            if (ranges_overlap_u32(vaddr, end_excl, curr->mem->heap_start, curr->mem->prog_break)) {
                vaddr = align_up_4k_u32(curr->mem->prog_break + 0x100000u);
                continue;
            }
        }

        mmap_area_t* ov = mmap_find_overlap(curr, vaddr, end_excl);
        if (ov) {
            vaddr = align_up_4k_u32(ov->vaddr_end);
            continue;
        }

        found = 1;
        break;
    }

    if (!found) {
        file_desc_release(d);
        regs->eax = 0;
        return;
    }

    if (!curr->mem) {
        regs->eax = 0;
        return;
    }

    mmap_area_t* area = (mmap_area_t*)kmalloc(sizeof(mmap_area_t));
    if (!area) {
        file_desc_release(d);
        regs->eax = 0;
        return;
    }

    area->vaddr_start = vaddr;
    area->vaddr_end = vaddr + size_aligned;
    area->file_offset = 0;
    area->length = size;
    area->file = d->node;
    vfs_node_retain(area->file);
    area->file_size = (d->node->size < size) ? d->node->size : size;
    area->map_flags = map_kind;

    file_desc_release(d);

    if (vaddr + size_aligned < vaddr || vaddr + size_aligned > 0xC0000000u) {
        regs->eax = 0;
        return;
    }

    area->next = curr->mem->mmap_list;
    curr->mem->mmap_list = area;
    curr->mem->mmap_top = vaddr + size_aligned;

    regs->eax = vaddr;
}

static void syscall_munmap(registers_t* regs, task_t* curr) {
    uint32_t vaddr = regs->ebx;
    uint32_t len = regs->ecx;

    if (!curr->mem || !curr->mem->page_dir) {
        regs->eax = (uint32_t)-1;
        return;
    }

    if (vaddr & 0xFFFu) {
        regs->eax = (uint32_t)-1;
        return;
    }
    if (len == 0) {
        regs->eax = (uint32_t)-1;
        return;
    }
    if (vaddr + len < vaddr) {
        regs->eax = (uint32_t)-1;
        return;
    }

    uint32_t aligned_len = (len + 4095u) & ~4095u;
    uint32_t vaddr_end = vaddr + aligned_len;

    uint32_t scan = vaddr;
    while (scan < vaddr_end) {
        mmap_area_t* a = mmap_find_area(curr, scan);
        if (!a || a->vaddr_end <= scan) {
            regs->eax = (uint32_t)-1;
            return;
        }
        scan = a->vaddr_end;
    }

    int result = 0;
    mmap_area_t* prev = 0;
    mmap_area_t* m = curr->mem->mmap_list;

    while (m) {
        mmap_area_t* next_node = m->next;

        uint32_t u_start = vaddr;
        uint32_t u_end = vaddr_end;
        uint32_t m_start = m->vaddr_start;
        uint32_t m_end = m->vaddr_end;

        if (u_end <= m_start || u_start >= m_end) {
            prev = m;
            m = next_node;
            continue;
        }

        result = 0;
        uint32_t o_start = (u_start > m_start) ? u_start : m_start;
        uint32_t o_end = (u_end < m_end) ? u_end : m_end;

        if (o_start > m_start && o_end < m_end) {
            mmap_area_t* new_right = (mmap_area_t*)kmalloc(sizeof(mmap_area_t));
            if (!new_right) {
                result = -1;
                break;
            }

            uint32_t orig_file_size = m->file_size;
            uint32_t left_len = o_start - m_start;
            uint32_t right_len = m_end - o_end;
            uint32_t cut_before_right = o_end - m_start;

            new_right->vaddr_start = o_end;
            new_right->vaddr_end = m_end;
            new_right->length = right_len;
            new_right->file = m->file;
            new_right->map_flags = m->map_flags;
            new_right->next = m->next;

            if (m->file) {
                new_right->file_offset = m->file_offset + cut_before_right;
                vfs_node_retain(m->file);
            } else {
                new_right->file_offset = 0;
            }

            uint32_t right_file_size = 0;
            if (orig_file_size > cut_before_right) right_file_size = orig_file_size - cut_before_right;
            if (right_file_size > right_len) right_file_size = right_len;
            new_right->file_size = right_file_size;

            m->vaddr_end = o_start;
            m->length = left_len;

            uint32_t left_file_size = orig_file_size;
            if (left_file_size > left_len) left_file_size = left_len;
            m->file_size = left_file_size;

            m->next = new_right;

            for (uint32_t curr_v = o_start; curr_v < o_end; curr_v += 4096u) {
                uint32_t pte;
                if (!paging_get_present_pte(curr->mem->page_dir, curr_v, &pte)) continue;
                if ((pte & 4u) == 0) continue;

                paging_map(curr->mem->page_dir, curr_v, 0, 0);

                uint32_t phys = pte & ~0xFFFu;
                if ((pte & 0x200u) == 0 && curr->mem->mem_pages > 0) curr->mem->mem_pages--;
                if (phys && (pte & 0x200u) == 0) {
                    pmm_free_block((void*)phys);
                }
            }

            prev = new_right;
            m = new_right->next;
            continue;
        }

        for (uint32_t curr_v = o_start; curr_v < o_end; curr_v += 4096u) {
            uint32_t pte;
            if (!paging_get_present_pte(curr->mem->page_dir, curr_v, &pte)) continue;
            if ((pte & 4u) == 0) continue;

            paging_map(curr->mem->page_dir, curr_v, 0, 0);

            uint32_t phys = pte & ~0xFFFu;
            if ((pte & 0x200u) == 0 && curr->mem->mem_pages > 0) curr->mem->mem_pages--;
            if (phys && (pte & 0x200u) == 0) {
                pmm_free_block((void*)phys);
            }
        }

        if (o_start == m_start && o_end == m_end) {
            if (prev) prev->next = next_node;
            else curr->mem->mmap_list = next_node;

            if (m->file) {
                vfs_node_release(m->file);
            }
            kfree(m);

            m = next_node;
            continue;
        }

        if (o_start == m_start && o_end < m_end) {
            uint32_t cut_len = o_end - m_start;
            uint32_t new_len = m_end - o_end;

            m->vaddr_start = o_end;
            m->length = new_len;

            if (m->file) m->file_offset += cut_len;
            if (m->file_size > cut_len) m->file_size -= cut_len;
            else m->file_size = 0;
            if (m->file_size > new_len) m->file_size = new_len;

            prev = m;
            m = next_node;
            continue;
        }

        if (o_start > m_start && o_end == m_end) {
            uint32_t new_len = o_start - m_start;

            m->vaddr_end = o_start;
            m->length = new_len;

            if (m->file_size > new_len) m->file_size = new_len;

            prev = m;
            m = next_node;
            continue;
        }

        prev = m;
        m = next_node;
    }

    regs->eax = (uint32_t)result;
}

static void syscall_stat(registers_t* regs, task_t* curr) {
    char* path = (char*)regs->ebx;
    user_stat_t* u_stat = (user_stat_t*)regs->ecx;

    if (!check_user_buffer(curr, path, 1) || !check_user_buffer(curr, u_stat, sizeof(user_stat_t))) {
        regs->eax = (uint32_t)-1;
        return;
    }

    int inode_idx = yulafs_lookup(path);
    if (inode_idx < 0) {
        regs->eax = (uint32_t)-1;
        return;
    }

    yfs_inode_t k_inode;
    yulafs_stat((yfs_ino_t)inode_idx, &k_inode);
    u_stat->type = k_inode.type;
    u_stat->size = k_inode.size;
    regs->eax = 0;
}

static void syscall_get_fs_info(registers_t* regs, task_t* curr) {
    user_fs_info_t* u_info = (user_fs_info_t*)regs->ebx;

    if (!check_user_buffer(curr, u_info, sizeof(user_fs_info_t))) {
        regs->eax = (uint32_t)-1;
        return;
    }

    uint32_t t, f, b;
    yulafs_get_filesystem_info(&t, &f, &b);

    u_info->total_blocks = t;
    u_info->free_blocks = f;
    u_info->block_size = b;

    regs->eax = 0;
}

static void syscall_rename(registers_t* regs, task_t* curr) {
    char* oldp = (char*)regs->ebx;
    char* newp = (char*)regs->ecx;

    if (check_user_buffer(curr, oldp, 1) && check_user_buffer(curr, newp, 1)) {
        regs->eax = (uint32_t)yulafs_rename(oldp, newp);
    } else {
        regs->eax = (uint32_t)-1;
    }
}

static void syscall_spawn_process(registers_t* regs, task_t* curr) {
    const char* path = (const char*)regs->ebx;
    int argc = (int)regs->ecx;
    char** argv = (char**)regs->edx;

    if (argc < 0 || argc > 64) {
        regs->eax = (uint32_t)-1;
        return;
    }

    if (!check_user_buffer(curr, path, 1)) {
        regs->eax = (uint32_t)-1;
        return;
    }

    if (argc > 0) {
        if (!argv || !check_user_buffer(curr, argv, (uint32_t)argc * sizeof(char*))) {
            regs->eax = (uint32_t)-1;
            return;
        }
        for (int i = 0; i < argc; i++) {
            char* a = argv[i];
            if (!check_user_buffer(curr, a, 1)) {
                regs->eax = (uint32_t)-1;
                return;
            }
        }
    }

    task_t* child = proc_spawn_elf(path, argc, argv);
    regs->eax = child ? (uint32_t)child->pid : (uint32_t)-1;
}

static void syscall_waitpid(registers_t* regs, task_t* curr) {
    uint32_t pid = (uint32_t)regs->ebx;
    int* status_ptr = (int*)regs->ecx;

    if (pid == 0) {
        regs->eax = (uint32_t)-1;
        return;
    }

    if (status_ptr && !check_user_buffer(curr, status_ptr, sizeof(int))) {
        regs->eax = (uint32_t)-1;
        return;
    }

    int rc = proc_waitpid(pid, status_ptr);
    regs->eax = (rc == 0) ? pid : (uint32_t)-1;
}

static void syscall_getdents(registers_t* regs, task_t* curr) {
    int fd = (int)regs->ebx;
    void* buf = (void*)regs->ecx;
    uint32_t size = (uint32_t)regs->edx;

    if (!check_user_buffer(curr, buf, size)) {
        regs->eax = (uint32_t)-1;
        return;
    }

    regs->eax = (uint32_t)vfs_getdents(fd, buf, size);
}

static void syscall_fstatat(registers_t* regs, task_t* curr) {
    int dirfd = (int)regs->ebx;
    char* name = (char*)regs->ecx;
    user_stat_t* u_stat = (user_stat_t*)regs->edx;

    if (!check_user_buffer(curr, name, 1) || !check_user_buffer(curr, u_stat, sizeof(user_stat_t))) {
        regs->eax = (uint32_t)-1;
        return;
    }

    regs->eax = (uint32_t)vfs_fstatat(dirfd, name, u_stat);
}

static void syscall_fb_map(registers_t* regs, task_t* curr) {
    if (!curr->mem || !curr->mem->page_dir) {
        regs->eax = 0;
        return;
    }

    if (fb_get_owner_pid() != curr->pid) {
        regs->eax = 0;
        return;
    }

    const uint32_t user_vaddr_start = 0xB1000000u;

    if (virtio_gpu_is_active()) {
        const virtio_gpu_fb_t* vfb = virtio_gpu_get_fb();
        if (!vfb || !vfb->fb_ptr || vfb->width == 0 || vfb->height == 0 || vfb->pitch == 0) {
            regs->eax = 0;
            return;
        }

        uint64_t virtio_min_pitch64 = (uint64_t)vfb->width * 4ull;
        if (virtio_min_pitch64 > 0xFFFFFFFFu || vfb->pitch < (uint32_t)virtio_min_pitch64) {
            regs->eax = 0;
            return;
        }

        uint64_t fb_size64 = (uint64_t)vfb->pitch * (uint64_t)vfb->height;
        if (fb_size64 == 0 || fb_size64 > 0xFFFFFFFFu || vfb->size_bytes < (uint32_t)fb_size64) {
            regs->eax = 0;
            return;
        }

        uint32_t fb_base = vfb->fb_phys;
        uint32_t page_off = fb_base & 0xFFFu;
        uint32_t fb_page = fb_base & ~0xFFFu;
        uint32_t fb_size = (uint32_t)fb_size64;

        if (fb_size > 0xFFFFFFFFu - page_off) {
            regs->eax = 0;
            return;
        }

        uint32_t map_size = fb_size + page_off;
        uint32_t pages = (map_size + 0xFFFu) / 4096u;

        if (curr->mem->fbmap_pages > 0) {
            for (uint32_t i = 0; i < curr->mem->fbmap_pages; i++) {
                uint32_t v = user_vaddr_start + i * 4096u;
                if (paging_is_user_accessible(curr->mem->page_dir, v)) {
                    paging_map(curr->mem->page_dir, v, 0, 0);
                }
            }
            if (curr->mem->mem_pages >= curr->mem->fbmap_pages) curr->mem->mem_pages -= curr->mem->fbmap_pages;
            else curr->mem->mem_pages = 0;
            curr->mem->fbmap_pages = 0;
            curr->mem->fbmap_user_ptr = 0;
            curr->mem->fbmap_size_bytes = 0;
            curr->mem->fbmap_is_virtio = 0;
        }

        uint32_t flags = PTE_PRESENT | PTE_RW | PTE_USER | 0x200u;
        if (paging_pat_is_supported()) {
            flags |= PTE_PAT;
        }

        for (uint32_t i = 0; i < pages; i++) {
            uint32_t offset = i * 4096u;
            paging_map(curr->mem->page_dir, user_vaddr_start + offset, fb_page + offset, flags);
            curr->mem->mem_pages++;
        }

        curr->mem->fbmap_pages = pages;
        curr->mem->fbmap_user_ptr = user_vaddr_start + page_off;
        curr->mem->fbmap_size_bytes = fb_size;
        curr->mem->fbmap_is_virtio = 1;

        __asm__ volatile("mov %%cr3, %%eax; mov %%eax, %%cr3" ::: "eax");
        regs->eax = user_vaddr_start + page_off;
        return;
    }

    if (!fb_ptr || fb_width == 0 || fb_height == 0 || fb_pitch == 0) {
        regs->eax = 0;
        return;
    }

    uint32_t fb_base = (uint32_t)fb_ptr;
    uint32_t page_off = fb_base & 0xFFFu;
    uint32_t fb_page = fb_base & ~0xFFFu;

    uint64_t fb_size64 = (uint64_t)fb_pitch * (uint64_t)fb_height;
    if (fb_size64 == 0 || fb_size64 > 0xFFFFFFFFu) {
        regs->eax = 0;
        return;
    }

    uint32_t fb_size = (uint32_t)fb_size64;
    if (fb_size > 0xFFFFFFFFu - page_off) {
        regs->eax = 0;
        return;
    }

    uint32_t map_size = fb_size + page_off;
    uint32_t pages = (map_size + 0xFFFu) / 4096u;

    if (curr->mem->fbmap_pages > 0) {
        for (uint32_t i = 0; i < curr->mem->fbmap_pages; i++) {
            uint32_t v = user_vaddr_start + i * 4096u;
            if (paging_is_user_accessible(curr->mem->page_dir, v)) {
                paging_map(curr->mem->page_dir, v, 0, 0);
            }
        }
        if (curr->mem->mem_pages >= curr->mem->fbmap_pages) curr->mem->mem_pages -= curr->mem->fbmap_pages;
        else curr->mem->mem_pages = 0;
        curr->mem->fbmap_pages = 0;
        curr->mem->fbmap_user_ptr = 0;
        curr->mem->fbmap_size_bytes = 0;
        curr->mem->fbmap_is_virtio = 0;
    }

    uint32_t flags = PTE_PRESENT | PTE_RW | PTE_USER | 0x200u;
    if (paging_pat_is_supported()) {
        flags |= PTE_PAT;
    }

    for (uint32_t i = 0; i < pages; i++) {
        uint32_t offset = i * 4096u;
        paging_map(curr->mem->page_dir, user_vaddr_start + offset, fb_page + offset, flags);
        curr->mem->mem_pages++;
    }

    curr->mem->fbmap_pages = pages;
    curr->mem->fbmap_user_ptr = user_vaddr_start + page_off;
    curr->mem->fbmap_size_bytes = fb_size;
    curr->mem->fbmap_is_virtio = 0;

    __asm__ volatile("mov %%cr3, %%eax; mov %%eax, %%cr3" ::: "eax");
    regs->eax = user_vaddr_start + page_off;
}

static void syscall_fb_acquire(registers_t* regs, task_t* curr) {
    regs->eax = (uint32_t)fb_acquire(curr->pid);
}

static void syscall_fb_release(registers_t* regs, task_t* curr) {
    const uint32_t user_vaddr_start = 0xB1000000u;

    if (curr->mem && curr->mem->fbmap_pages > 0) {
        for (uint32_t i = 0; i < curr->mem->fbmap_pages; i++) {
            uint32_t v = user_vaddr_start + i * 4096u;
            if (curr->mem->page_dir && paging_is_user_accessible(curr->mem->page_dir, v)) {
                paging_map(curr->mem->page_dir, v, 0, 0);
            }
        }
        if (curr->mem->mem_pages >= curr->mem->fbmap_pages) curr->mem->mem_pages -= curr->mem->fbmap_pages;
        else curr->mem->mem_pages = 0;
        curr->mem->fbmap_pages = 0;
    }

    if (curr->mem) {
        curr->mem->fbmap_user_ptr = 0;
        curr->mem->fbmap_size_bytes = 0;
        curr->mem->fbmap_is_virtio = 0;
    }

    __asm__ volatile("mov %%cr3, %%eax; mov %%eax, %%cr3" ::: "eax");
    regs->eax = (uint32_t)fb_release(curr->pid);
}

static void syscall_shm_create(registers_t* regs, task_t* curr) {
    uint32_t size = (uint32_t)regs->ebx;
    if (size == 0) {
        regs->eax = (uint32_t)-1;
        return;
    }

    uint32_t pages = (size + 4095u) / 4096u;
    if (pages == 0 || pages > 16384u) {
        regs->eax = (uint32_t)-1;
        return;
    }

    vfs_node_t* node = shm_create_node(size);
    if (!node) {
        regs->eax = (uint32_t)-1;
        return;
    }

    file_desc_t* d = 0;
    int fd = proc_fd_alloc(curr, &d);
    if (fd < 0 || !d) {
        vfs_node_release(node);
        regs->eax = (uint32_t)-1;
        return;
    }

    d->node = node;
    d->offset = 0;
    d->flags = 0;
    regs->eax = (uint32_t)fd;
}

static void syscall_pipe_try_read(registers_t* regs, task_t* curr) {
    int fd = (int)regs->ebx;
    void* buf = (void*)regs->ecx;
    uint32_t size = (uint32_t)regs->edx;

    if (!check_user_buffer(curr, buf, size)) {
        regs->eax = (uint32_t)-1;
        return;
    }

    file_desc_t* d = proc_fd_get(curr, fd);
    if (!d || !d->node) {
        regs->eax = (uint32_t)-1;
        return;
    }

    regs->eax = (uint32_t)pipe_read_nonblock(d->node, size, buf);
    file_desc_release(d);
}

static void syscall_pipe_try_write(registers_t* regs, task_t* curr) {
    int fd = (int)regs->ebx;
    const void* buf = (const void*)regs->ecx;
    uint32_t size = (uint32_t)regs->edx;

    if (!check_user_buffer(curr, buf, size)) {
        regs->eax = (uint32_t)-1;
        return;
    }

    file_desc_t* d = proc_fd_get(curr, fd);
    if (!d || !d->node) {
        regs->eax = (uint32_t)-1;
        return;
    }

    regs->eax = (uint32_t)pipe_write_nonblock(d->node, size, buf);
    file_desc_release(d);
}

static void syscall_kbd_try_read(registers_t* regs, task_t* curr) {
    char* out = (char*)regs->ebx;
    if (!check_user_buffer(curr, out, 1)) {
        regs->eax = (uint32_t)-1;
        return;
    }

    uint32_t owner_pid = fb_get_owner_pid();
    if (owner_pid != 0) {
        if (curr->pid != owner_pid) {
            regs->eax = 0;
            return;
        }
    } else {
        uint32_t focus_pid = input_focus_get_pid();
        if (focus_pid > 0 && curr->pid != focus_pid) {
            regs->eax = 0;
            return;
        }
    }

    char c = 0;
    if (!kbd_try_read_char(&c)) {
        regs->eax = 0;
        return;
    }

    out[0] = c;
    regs->eax = 1;
}

static void syscall_ipc_listen(registers_t* regs, task_t* curr) {
    const char* u_name = (const char*)regs->ebx;
    int* out_fds = (int*)regs->ecx;

    if (out_fds && !ensure_user_buffer_writable_mappable(curr, out_fds, (uint32_t)sizeof(int) * 2u)) {
        regs->eax = (uint32_t)-1;
        return;
    }

    char name[32];
    if (copy_user_str_bounded(curr, name, (uint32_t)sizeof(name), u_name) != 0) {
        regs->eax = (uint32_t)-1;
        return;
    }

    vfs_node_t* node = ipc_listen_create(name);
    if (!node) {
        regs->eax = (uint32_t)-1;
        return;
    }

    file_desc_t* d = 0;
    int fd = proc_fd_alloc(curr, &d);
    if (fd < 0 || !d) {
        vfs_node_release(node);
        regs->eax = (uint32_t)-1;
        return;
    }

    d->node = node;
    d->offset = 0;
    d->flags = 0;
    regs->eax = (uint32_t)fd;
}

static void syscall_ipc_accept(registers_t* regs, task_t* curr) {
    int listen_fd = (int)regs->ebx;
    int* out_fds = (int*)regs->ecx;

    if (!ensure_user_buffer_writable_mappable(curr, out_fds, (uint32_t)sizeof(int) * 2u)) {
        regs->eax = (uint32_t)-1;
        return;
    }

    file_desc_t* lf = proc_fd_get(curr, listen_fd);
    if (!lf || !lf->node) {
        regs->eax = (uint32_t)-1;
        return;
    }
    if ((lf->node->flags & VFS_FLAG_IPC_LISTEN) == 0) {
        file_desc_release(lf);
        regs->eax = (uint32_t)-1;
        return;
    }

    vfs_node_t* in_r = 0;
    vfs_node_t* out_w = 0;
    int ar = ipc_accept(lf->node, &in_r, &out_w);
    file_desc_release(lf);
    if (ar <= 0) {
        regs->eax = (uint32_t)ar;
        return;
    }

    file_desc_t* fr = 0;
    file_desc_t* fw = 0;

    int fd_r = proc_fd_alloc(curr, &fr);
    if (fd_r < 0 || !fr) {
        vfs_node_release(in_r);
        vfs_node_release(out_w);
        regs->eax = (uint32_t)-1;
        return;
    }

    int fd_w = proc_fd_alloc(curr, &fw);
    if (fd_w < 0 || !fw) {
        file_desc_t* tmp = 0;
        (void)proc_fd_remove(curr, fd_r, &tmp);
        file_desc_release(tmp);

        vfs_node_release(in_r);
        vfs_node_release(out_w);
        regs->eax = (uint32_t)-1;
        return;
    }

    fr->node = in_r;
    fr->offset = 0;
    fr->flags = 0;

    fw->node = out_w;
    fw->offset = 0;
    fw->flags = 0;

    out_fds[0] = fd_r;
    out_fds[1] = fd_w;
    regs->eax = 1;
}

static void syscall_ipc_connect(registers_t* regs, task_t* curr) {
    const char* u_name = (const char*)regs->ebx;
    int* out_fds = (int*)regs->ecx;

    if (!ensure_user_buffer_writable_mappable(curr, out_fds, (uint32_t)sizeof(int) * 2u)) {
        regs->eax = (uint32_t)-1;
        return;
    }

    char name[32];
    if (copy_user_str_bounded(curr, name, (uint32_t)sizeof(name), u_name) != 0) {
        regs->eax = (uint32_t)-1;
        return;
    }

    vfs_node_t* in_w = 0;
    vfs_node_t* out_r = 0;
    void* pending = 0;
    if (ipc_connect(name, &in_w, &out_r, &pending) != 0) {
        regs->eax = (uint32_t)-1;
        return;
    }

    file_desc_t* fr = 0;
    file_desc_t* fw = 0;

    int fd_r = proc_fd_alloc(curr, &fr);
    if (fd_r < 0 || !fr) {
        ipc_connect_cancel(pending);
        vfs_node_release(in_w);
        vfs_node_release(out_r);
        regs->eax = (uint32_t)-1;
        return;
    }

    int fd_w = proc_fd_alloc(curr, &fw);
    if (fd_w < 0 || !fw) {
        file_desc_t* tmp = 0;
        (void)proc_fd_remove(curr, fd_r, &tmp);
        file_desc_release(tmp);

        ipc_connect_cancel(pending);
        vfs_node_release(in_w);
        vfs_node_release(out_r);
        regs->eax = (uint32_t)-1;
        return;
    }

    fr->node = out_r;
    fr->offset = 0;
    fr->flags = 0;

    fw->node = in_w;
    fw->offset = 0;
    fw->flags = 0;

    out_fds[0] = fd_r;
    out_fds[1] = fd_w;
    regs->eax = 0;
}

static void syscall_shm_create_named(registers_t* regs, task_t* curr) {
    const char* u_name = (const char*)regs->ebx;
    uint32_t size = (uint32_t)regs->ecx;

    if (size == 0) {
        regs->eax = (uint32_t)-1;
        return;
    }
    uint32_t pages = (size + 4095u) / 4096u;
    if (pages == 0 || pages > 16384u) {
        regs->eax = (uint32_t)-1;
        return;
    }

    char name[32];
    if (copy_user_str_bounded(curr, name, (uint32_t)sizeof(name), u_name) != 0) {
        regs->eax = (uint32_t)-1;
        return;
    }

    vfs_node_t* node = shm_create_named_node(name, size);
    if (!node) {
        regs->eax = (uint32_t)-1;
        return;
    }

    file_desc_t* d = 0;
    int fd = proc_fd_alloc(curr, &d);
    if (fd < 0 || !d) {
        vfs_node_release(node);
        regs->eax = (uint32_t)-1;
        return;
    }

    d->node = node;
    d->offset = 0;
    d->flags = 0;
    regs->eax = (uint32_t)fd;
}

static void syscall_shm_open_named(registers_t* regs, task_t* curr) {
    const char* u_name = (const char*)regs->ebx;

    char name[32];
    if (copy_user_str_bounded(curr, name, (uint32_t)sizeof(name), u_name) != 0) {
        regs->eax = (uint32_t)-1;
        return;
    }

    vfs_node_t* node = shm_open_named_node(name);
    if (!node) {
        regs->eax = (uint32_t)-1;
        return;
    }

    file_desc_t* d = 0;
    int fd = proc_fd_alloc(curr, &d);
    if (fd < 0 || !d) {
        vfs_node_release(node);
        regs->eax = (uint32_t)-1;
        return;
    }

    d->node = node;
    d->offset = 0;
    d->flags = 0;
    regs->eax = (uint32_t)fd;
}

static void syscall_shm_unlink_named(registers_t* regs, task_t* curr) {
    const char* u_name = (const char*)regs->ebx;

    char name[32];
    if (copy_user_str_bounded(curr, name, (uint32_t)sizeof(name), u_name) != 0) {
        regs->eax = (uint32_t)-1;
        return;
    }

    regs->eax = (uint32_t)shm_unlink_named(name);
}

static void syscall_futex_wait(registers_t* regs, task_t* curr) {
    volatile const uint32_t* uaddr = (volatile const uint32_t*)regs->ebx;
    uint32_t expected = (uint32_t)regs->ecx;

    if (((uint32_t)uaddr & 3u) != 0) {
        regs->eax = (uint32_t)-1;
        return;
    }

    if (!check_user_buffer_present(curr, (const void*)uaddr, 4u)) {
        regs->eax = (uint32_t)-1;
        return;
    }

    if (!curr->mem || !curr->mem->page_dir) {
        regs->eax = (uint32_t)-1;
        return;
    }

    uint32_t phys = paging_get_phys(curr->mem->page_dir, (uint32_t)uaddr);
    if (!phys) {
        regs->eax = (uint32_t)-1;
        return;
    }
    uint32_t key = phys & ~3u;

    semaphore_t* sem = futex_get_sem(key);
    if (!sem) {
        regs->eax = (uint32_t)-1;
        return;
    }

    regs->eax = (uint32_t)futex_sem_wait(sem, uaddr, expected);
}

static void syscall_futex_wake(registers_t* regs, task_t* curr) {
    volatile const uint32_t* uaddr = (volatile const uint32_t*)regs->ebx;
    uint32_t max_wake = (uint32_t)regs->ecx;

    if (((uint32_t)uaddr & 3u) != 0) {
        regs->eax = (uint32_t)-1;
        return;
    }

    if (!check_user_buffer_present(curr, (const void*)uaddr, 4u)) {
        regs->eax = (uint32_t)-1;
        return;
    }

    if (!curr->mem || !curr->mem->page_dir) {
        regs->eax = (uint32_t)-1;
        return;
    }

    uint32_t phys = paging_get_phys(curr->mem->page_dir, (uint32_t)uaddr);
    if (!phys) {
        regs->eax = (uint32_t)-1;
        return;
    }
    uint32_t key = phys & ~3u;

    semaphore_t* sem = futex_lookup_sem(key);
    if (!sem) {
        regs->eax = 0;
        return;
    }

    regs->eax = (uint32_t)futex_sem_wake(sem, max_wake);
}

static void syscall_ioctl(registers_t* regs, task_t* curr) {
    int fd = (int)regs->ebx;
    uint32_t req = (uint32_t)regs->ecx;
    void* arg = (void*)regs->edx;

    uint32_t sz = _YOS_IOC_SIZE(req);
    uint32_t dir = _YOS_IOC_DIR(req);

    if (sz != 0u) {
        if (!arg) {
            regs->eax = (uint32_t)-1;
            return;
        }

        if (dir & _YOS_IOC_READ) {
            if (!check_user_buffer_writable_present(curr, arg, sz)) {
                regs->eax = (uint32_t)-1;
                return;
            }
        } else {
            if (!check_user_buffer_present(curr, arg, sz)) {
                regs->eax = (uint32_t)-1;
                return;
            }
        }
    }

    regs->eax = (uint32_t)vfs_ioctl(fd, req, arg);
}

static void syscall_uptime_ms(registers_t* regs, task_t* curr) {
    (void)curr;
    uint64_t ms64 = ((uint64_t)timer_ticks * 1000ull) / 15000ull;
    if (ms64 > 0xFFFFFFFFull) ms64 = 0xFFFFFFFFull;
    regs->eax = (uint32_t)ms64;
}

static void syscall_proc_list(registers_t* regs, task_t* curr) {
    yos_proc_info_t* u_buf = (yos_proc_info_t*)regs->ebx;
    uint32_t cap = (uint32_t)regs->ecx;

    if (!u_buf || cap == 0) {
        regs->eax = (uint32_t)-1;
        return;
    }

    if (cap > (0xFFFFFFFFu / (uint32_t)sizeof(*u_buf))) {
        regs->eax = (uint32_t)-1;
        return;
    }

    uint32_t bytes = cap * (uint32_t)sizeof(*u_buf);
    if (!check_user_buffer_writable_present(curr, (void*)u_buf, bytes)) {
        regs->eax = (uint32_t)-1;
        return;
    }

    regs->eax = (uint32_t)proc_list_snapshot(u_buf, cap);
}

static void syscall_chdir(registers_t* regs, task_t* curr) {
    const char* u_path = (const char*)regs->ebx;
    char path[256];
    if (copy_user_str_bounded(curr, path, (uint32_t)sizeof(path), u_path) != 0) {
        regs->eax = (uint32_t)-1;
        return;
    }

    int ino = yulafs_lookup(path);
    if (ino <= 0) {
        regs->eax = (uint32_t)-1;
        return;
    }

    yfs_inode_t info;
    yulafs_stat((yfs_ino_t)ino, &info);
    if (info.type != YFS_TYPE_DIR) {
        regs->eax = (uint32_t)-1;
        return;
    }

    curr->cwd_inode = (uint32_t)ino;
    regs->eax = 0;
}

static void syscall_getcwd(registers_t* regs, task_t* curr) {
    char* u_buf = (char*)regs->ebx;
    uint32_t size = (uint32_t)regs->ecx;

    if (!u_buf || size == 0) {
        regs->eax = (uint32_t)-1;
        return;
    }

    if (!check_user_buffer_writable_present(curr, u_buf, size)) {
        regs->eax = (uint32_t)-1;
        return;
    }

    yfs_ino_t cur = (yfs_ino_t)(curr->cwd_inode ? curr->cwd_inode : 1u);
    if (cur == 1u) {
        if (size < 2u) {
            regs->eax = (uint32_t)-1;
            return;
        }
        u_buf[0] = '/';
        u_buf[1] = '\0';
        regs->eax = 1;
        return;
    }

    char parts[64][YFS_NAME_MAX];
    uint32_t depth = 0;

    while (cur != 1u) {
        if (depth >= (uint32_t)(sizeof(parts) / sizeof(parts[0]))) {
            regs->eax = (uint32_t)-1;
            return;
        }

        int parent_i = yulafs_lookup_in_dir(cur, "..");
        if (parent_i <= 0) {
            regs->eax = (uint32_t)-1;
            return;
        }

        yfs_ino_t parent = (yfs_ino_t)parent_i;

        if (yulafs_find_child_name_in_dir(parent, cur, parts[depth], (uint32_t)sizeof(parts[depth])) != 0) {
            regs->eax = (uint32_t)-1;
            return;
        }

        depth++;
        if (parent == cur) {
            regs->eax = (uint32_t)-1;
            return;
        }
        cur = parent;
    }

    char kbuf[1024];
    uint32_t len = 0;
    kbuf[len++] = '/';

    for (uint32_t i = depth; i > 0; i--) {
        const char* name = parts[i - 1u];
        uint32_t name_len = (uint32_t)strlen(name);
        if (name_len == 0) {
            regs->eax = (uint32_t)-1;
            return;
        }
        if (len + name_len + 1u >= (uint32_t)sizeof(kbuf)) {
            regs->eax = (uint32_t)-1;
            return;
        }
        memcpy(&kbuf[len], name, name_len);
        len += name_len;
        if (i > 1u) {
            kbuf[len++] = '/';
        }
    }

    if (len + 1u > size) {
        regs->eax = (uint32_t)-1;
        return;
    }

    kbuf[len] = '\0';
    memcpy(u_buf, kbuf, len + 1u);
    regs->eax = (int)len;
}

static void syscall_fb_present(registers_t* regs, task_t* curr) {
    const fb_present_req_t* u_req = (const fb_present_req_t*)regs->ebx;

    if (fb_get_owner_pid() != curr->pid) {
        regs->eax = (uint32_t)-1;
        return;
    }

    const int virtio_active = virtio_gpu_is_active();
    const virtio_gpu_fb_t* virtio_fb = virtio_active ? virtio_gpu_get_fb() : 0;

    uint32_t dst_width = fb_width;
    uint32_t dst_height = fb_height;
    uint32_t dst_pitch = fb_pitch;
    uint32_t* dst_ptr = fb_ptr;

    if (virtio_active) {
        if (!virtio_fb || !virtio_fb->fb_ptr || virtio_fb->width == 0 || virtio_fb->height == 0 || virtio_fb->pitch == 0) {
            regs->eax = (uint32_t)-1;
            return;
        }

        uint64_t virtio_min_pitch64 = (uint64_t)virtio_fb->width * 4ull;
        if (virtio_min_pitch64 > 0xFFFFFFFFu || virtio_fb->pitch < (uint32_t)virtio_min_pitch64) {
            regs->eax = (uint32_t)-1;
            return;
        }

        uint64_t dst_size64 = (uint64_t)virtio_fb->pitch * (uint64_t)virtio_fb->height;
        if (dst_size64 == 0 || dst_size64 > 0xFFFFFFFFu || virtio_fb->size_bytes < (uint32_t)dst_size64) {
            regs->eax = (uint32_t)-1;
            return;
        }

        dst_width = virtio_fb->width;
        dst_height = virtio_fb->height;
        dst_pitch = virtio_fb->pitch;
        dst_ptr = virtio_fb->fb_ptr;
    } else {
        if (!fb_ptr || fb_width == 0 || fb_height == 0 || fb_pitch == 0) {
            regs->eax = (uint32_t)-1;
            return;
        }
    }

    if (!check_user_buffer(curr, u_req, (uint32_t)sizeof(*u_req))) {
        regs->eax = (uint32_t)-1;
        return;
    }

    if (!user_range_mappable(curr, (uintptr_t)u_req, (uintptr_t)u_req + (uintptr_t)sizeof(*u_req))) {
        regs->eax = (uint32_t)-1;
        return;
    }

    fb_present_req_t req = *u_req;

    if (!req.src || req.src_stride == 0) {
        regs->eax = (uint32_t)-1;
        return;
    }

    if (req.rect_count == 0) {
        regs->eax = 0;
        return;
    }

    if (!req.rects) {
        regs->eax = (uint32_t)-1;
        return;
    }

    if (req.rect_count > 4096u) {
        regs->eax = (uint32_t)-1;
        return;
    }

    uint32_t min_stride = dst_width * 4u;
    if (req.src_stride < min_stride) {
        regs->eax = (uint32_t)-1;
        return;
    }

    if (!check_user_buffer(curr, req.rects, req.rect_count * (uint32_t)sizeof(fb_rect_t))) {
        regs->eax = (uint32_t)-1;
        return;
    }

    {
        uintptr_t rs = (uintptr_t)req.rects;
        uintptr_t re = rs + (uintptr_t)req.rect_count * (uintptr_t)sizeof(fb_rect_t);
        if (re < rs || !user_range_mappable(curr, rs, re)) {
            regs->eax = (uint32_t)-1;
            return;
        }
    }

    if (!virtio_active) {
        const int src_is_mapped_fb = curr->mem && !curr->mem->fbmap_is_virtio && curr->mem->fbmap_user_ptr != 0 &&
                                    (uintptr_t)req.src == (uintptr_t)curr->mem->fbmap_user_ptr &&
                                    req.src_stride == dst_pitch;
        if (src_is_mapped_fb) {
            regs->eax = 0;
            return;
        }
    }

    if (!virtio_active) {
        uint32_t fpu_sz = fpu_state_size();
        if (fpu_sz == 0 || fpu_sz > 4096u) {
            regs->eax = (uint32_t)-1;
            return;
        }
    }

    const fb_rect_t* rects = req.rects;
    for (uint32_t i = 0; i < req.rect_count; i++) {
        fb_rect_t r = rects[i];
        if (r.w <= 0 || r.h <= 0) continue;

        int64_t x1_64 = (int64_t)r.x;
        int64_t y1_64 = (int64_t)r.y;
        int64_t x2_64 = (int64_t)r.x + (int64_t)r.w;
        int64_t y2_64 = (int64_t)r.y + (int64_t)r.h;

        if (x2_64 <= 0 || y2_64 <= 0) continue;
        if (x1_64 >= (int64_t)dst_width || y1_64 >= (int64_t)dst_height) continue;

        int x1 = (int)x1_64;
        int y1 = (int)y1_64;
        int x2 = (int)x2_64;
        int y2 = (int)y2_64;

        if (x1 < 0) x1 = 0;
        if (y1 < 0) y1 = 0;
        if (x2 > (int)dst_width) x2 = (int)dst_width;
        if (y2 > (int)dst_height) y2 = (int)dst_height;
        if (x1 >= x2 || y1 >= y2) continue;

        x1 &= ~3;
        x2 = (x2 + 3) & ~3;
        if (x2 > (int)dst_width) x2 = (int)dst_width;
        if (x1 >= x2) continue;

        uint32_t row_bytes = (uint32_t)((uint32_t)(x2 - x1) * 4u);
        const uint8_t* src_base = (const uint8_t*)req.src;

        if (virtio_active) {
            const int flush_only = curr->mem && curr->mem->fbmap_is_virtio && curr->mem->fbmap_user_ptr != 0 &&
                                    (uintptr_t)req.src == (uintptr_t)curr->mem->fbmap_user_ptr &&
                                    req.src_stride == dst_pitch;

            if (!flush_only) {
                uint8_t* dst_base = (uint8_t*)dst_ptr;
                for (int y = y1; y < y2; y++) {
                    uint64_t src_row_off = (uint64_t)(uint32_t)y * (uint64_t)req.src_stride + (uint64_t)(uint32_t)x1 * 4ull;
                    uint64_t src_row_addr = (uint64_t)(uintptr_t)src_base + src_row_off;
                    uint64_t src_row_end_excl = src_row_addr + (uint64_t)row_bytes;

                    if (src_row_end_excl < src_row_addr || src_row_addr < 0x08000000ull || src_row_end_excl > 0xC0000000ull) {
                        regs->eax = (uint32_t)-1;
                        return;
                    }

                    if (!user_range_mappable(curr, (uintptr_t)src_row_addr, (uintptr_t)src_row_end_excl)) {
                        regs->eax = (uint32_t)-1;
                        return;
                    }

                    prefault_user_read((const void*)(uintptr_t)src_row_addr, row_bytes);

                    uint64_t dst_row_off = (uint64_t)(uint32_t)y * (uint64_t)dst_pitch + (uint64_t)(uint32_t)x1 * 4ull;
                    memcpy(dst_base + dst_row_off, (const void*)(uintptr_t)src_row_addr, row_bytes);
                }
            }

            if (virtio_gpu_flush_rect(x1, y1, x2 - x1, y2 - y1) != 0) {
                regs->eax = (uint32_t)-1;
                return;
            }
        } else {
            for (int y = y1; y < y2; y++) {
                uint64_t row_off = (uint64_t)(uint32_t)y * (uint64_t)req.src_stride + (uint64_t)(uint32_t)x1 * 4ull;
                uint64_t row_addr = (uint64_t)(uintptr_t)src_base + row_off;
                uint64_t row_end_excl = row_addr + (uint64_t)row_bytes;

                if (row_end_excl < row_addr || row_addr < 0x08000000ull || row_end_excl > 0xC0000000ull) {
                    regs->eax = (uint32_t)-1;
                    return;
                }

                if (!user_range_mappable(curr, (uintptr_t)row_addr, (uintptr_t)row_end_excl)) {
                    regs->eax = (uint32_t)-1;
                    return;
                }

                prefault_user_read((const void*)(uintptr_t)row_addr, row_bytes);
            }

            cpu_t* cpu = cpu_current();
            if (!cpu || cpu->index < 0 || cpu->index >= MAX_CPUS) {
                regs->eax = (uint32_t)-1;
                return;
            }

            uint8_t* fpu_tmp = &fb_present_fpu_tmp[cpu->index][0];
            uint32_t irq_flags = irq_save_disable();
            fpu_save(fpu_tmp);

            smp_fb_present_rect(curr, req.src, req.src_stride, x1, y1, x2 - x1, y2 - y1);

            fpu_restore(fpu_tmp);
            irq_restore(irq_flags);
        }
    }

    regs->eax = 0;
}

static void syscall_poll(registers_t* regs, task_t* curr) {
    pollfd_t* u_fds = (pollfd_t*)regs->ebx;
    uint32_t nfds = (uint32_t)regs->ecx;
    int timeout_ms = (int)regs->edx;

    if (nfds > 4096u) {
        regs->eax = (uint32_t)-1;
        return;
    }

    uint32_t bytes = 0;
    if (nfds > 0) {
        if (__builtin_mul_overflow(nfds, (uint32_t)sizeof(pollfd_t), &bytes)) {
            regs->eax = (uint32_t)-1;
            return;
        }
        if (!check_user_buffer_present(curr, u_fds, bytes)) {
            regs->eax = (uint32_t)-1;
            return;
        }
    }

    pollfd_t* k_fds = 0;
    poll_waiter_t* waiters = 0;

    if (nfds > 0) {
        k_fds = (pollfd_t*)kmalloc(bytes);
        waiters = (poll_waiter_t*)kmalloc((uint32_t)sizeof(poll_waiter_t) * nfds);
        if (!k_fds || !waiters) {
            if (k_fds) kfree(k_fds);
            if (waiters) kfree(waiters);
            regs->eax = (uint32_t)-1;
            return;
        }
        memset(waiters, 0, (uint32_t)sizeof(poll_waiter_t) * nfds);
        prefault_user_read(u_fds, bytes);
        memcpy(k_fds, u_fds, bytes);
    }

    uint32_t end_tick = 0;
    int have_deadline = 0;
    if (timeout_ms > 0) {
        uint64_t t = (uint64_t)timer_ticks + (uint64_t)((uint32_t)timeout_ms) * 15ull;
        if (t > 0xFFFFFFFFull) t = 0xFFFFFFFFull;
        end_tick = (uint32_t)t;
        have_deadline = 1;
    }

    int result = 0;
    for (;;) {
        if (curr->pending_signals & (1u << 2)) {
            curr->pending_signals &= ~(1u << 2);
            result = -2;
            break;
        }

        if (nfds == 0) {
            if (timeout_ms == 0) {
                result = 0;
                break;
            }
            if (have_deadline && timer_ticks >= end_tick) {
                result = 0;
                break;
            }

            if (have_deadline) {
                proc_sleep_add(curr, end_tick);
            } else {
                curr->state = TASK_WAITING;
                sched_yield();
            }
            continue;
        }

        for (uint32_t i = 0; i < nfds; i++) {
            poll_waitq_unregister(&waiters[i]);
        }

        int ready = 0;
        for (uint32_t i = 0; i < nfds; i++) {
            pollfd_t* p = &k_fds[i];

            int32_t fd = p->fd;
            int16_t ev = p->events;
            int16_t rev = 0;

            if (fd < 0) {
                rev = POLLNVAL;
            } else {
                file_desc_t* d = proc_fd_get(curr, fd);
                if (!d || !d->node) {
                    rev = POLLNVAL;
                } else {
                    vfs_node_t* node = d->node;
                    if (node->flags & (VFS_FLAG_PIPE_READ | VFS_FLAG_PIPE_WRITE)) {
                        uint32_t avail = 0;
                        uint32_t space = 0;
                        int readers = 0;
                        int writers = 0;
                        if (pipe_poll_info(node, &avail, &space, &readers, &writers) == 0) {
                            if (node->flags & VFS_FLAG_PIPE_READ) {
                                if ((ev & POLLIN) && avail > 0) rev |= POLLIN;
                                if (writers == 0) rev |= POLLHUP;
                            }
                            if (node->flags & VFS_FLAG_PIPE_WRITE) {
                                if ((ev & POLLOUT) && readers > 0 && space > 0) rev |= POLLOUT;
                                if (readers == 0) rev |= POLLHUP;
                            }
                        }
                    } else if (node->flags & (VFS_FLAG_PTY_MASTER | VFS_FLAG_PTY_SLAVE)) {
                        uint32_t avail = 0;
                        uint32_t space = 0;
                        int peer_open = 0;
                        if (pty_poll_info(node, &avail, &space, &peer_open) == 0) {
                            if ((ev & POLLIN) && avail > 0) rev |= POLLIN;
                            if ((ev & POLLOUT) && peer_open > 0 && space > 0) rev |= POLLOUT;
                            if (peer_open == 0) rev |= POLLHUP;
                        }
                    } else if (node->flags & VFS_FLAG_IPC_LISTEN) {
                        if ((ev & POLLIN) && ipc_listen_poll_ready(node)) {
                            rev |= POLLIN;
                        }
                    } else if (ev & POLLIN) {
                        if (node->name[0] == 'k' && strcmp(node->name, "kbd") == 0) {
                            if (kbd_poll_ready(curr)) rev |= POLLIN;
                        } else if (node->name[0] == 'm' && strcmp(node->name, "mouse") == 0) {
                            if (mouse_poll_ready(curr)) rev |= POLLIN;
                        }
                    }
                }

                file_desc_release(d);
            }

            p->revents = rev;
            if (rev != 0) ready++;
        }

        if (ready > 0) {
            result = ready;
            break;
        }

        if (timeout_ms == 0) {
            result = 0;
            break;
        }

        if (have_deadline && timer_ticks >= end_tick) {
            result = 0;
            break;
        }

        for (uint32_t i = 0; i < nfds; i++) {
            pollfd_t* p = &k_fds[i];

            if (p->revents != 0) continue;

            int32_t fd = p->fd;
            int16_t ev = p->events;

            if (fd < 0) continue;
            if ((ev & (POLLIN | POLLOUT)) == 0) continue;

            file_desc_t* d = proc_fd_get(curr, fd);
            if (!d || !d->node) {
                file_desc_release(d);
                continue;
            }

            vfs_node_t* node = d->node;
            if (node->flags & (VFS_FLAG_PIPE_READ | VFS_FLAG_PIPE_WRITE)) {
                (void)pipe_poll_waitq_register(node, &waiters[i], curr);
            } else if (node->flags & (VFS_FLAG_PTY_MASTER | VFS_FLAG_PTY_SLAVE)) {
                (void)pty_poll_waitq_register(node, &waiters[i], curr);
            } else if (node->flags & VFS_FLAG_IPC_LISTEN) {
                if (ev & POLLIN) {
                    (void)ipc_listen_poll_waitq_register(node, &waiters[i], curr);
                }
            } else if (ev & POLLIN) {
                if (node->name[0] == 'k' && strcmp(node->name, "kbd") == 0) {
                    (void)kbd_poll_waitq_register(&waiters[i], curr);
                } else if (node->name[0] == 'm' && strcmp(node->name, "mouse") == 0) {
                    (void)mouse_poll_waitq_register(&waiters[i], curr);
                }
            }

            file_desc_release(d);
        }

        if (have_deadline) {
            proc_sleep_add(curr, end_tick);
        } else {
            curr->state = TASK_WAITING;
            sched_yield();
        }
    }

    if (waiters) {
        for (uint32_t i = 0; i < nfds; i++) {
            poll_waitq_unregister(&waiters[i]);
        }
    }

    if (result < 0 && k_fds) {
        for (uint32_t i = 0; i < nfds; i++) {
            k_fds[i].revents = 0;
        }
    }

    if (k_fds && bytes) {
        memcpy(u_fds, k_fds, bytes);
    }

    if (k_fds) kfree(k_fds);
    if (waiters) kfree(waiters);

    regs->eax = (uint32_t)result;
}

static void syscall_invalid(registers_t* regs, task_t* curr) {
    (void)curr;
    regs->eax = (uint32_t)-1;
}

static const syscall_fn_t syscall_table[] = {
    [0] = syscall_exit,
    [1] = syscall_print,
    [2] = syscall_getpid,
    [3] = syscall_open,
    [4] = syscall_read,
    [5] = syscall_write,
    [6] = syscall_close,
    [7] = syscall_sleep,
    [8] = syscall_sbrk,
    [9] = syscall_kill,
    [10] = syscall_invalid,
    [11] = syscall_usleep,
    [12] = syscall_get_mem_stats,
    [13] = syscall_mkdir,
    [14] = syscall_unlink,
    [15] = syscall_get_time,
    [16] = syscall_reboot,
    [17] = syscall_signal,
    [18] = syscall_sigreturn,
    [19] = syscall_invalid,
    [20] = syscall_clone,
    [21] = syscall_removed,
    [22] = syscall_removed,
    [23] = syscall_removed,
    [24] = syscall_invalid,
    [25] = syscall_set_clipboard,
    [26] = syscall_get_clipboard,
    [27] = syscall_set_term_mode,
    [28] = syscall_set_console_color,
    [29] = syscall_pipe,
    [30] = syscall_dup2,
    [31] = syscall_mmap,
    [32] = syscall_munmap,
    [33] = syscall_stat,
    [34] = syscall_get_fs_info,
    [35] = syscall_rename,
    [36] = syscall_spawn_process,
    [37] = syscall_waitpid,
    [38] = syscall_getdents,
    [39] = syscall_fstatat,
    [40] = syscall_fb_map,
    [41] = syscall_fb_acquire,
    [42] = syscall_fb_release,
    [43] = syscall_shm_create,
    [44] = syscall_pipe_try_read,
    [45] = syscall_pipe_try_write,
    [46] = syscall_kbd_try_read,
    [47] = syscall_ipc_listen,
    [48] = syscall_ipc_accept,
    [49] = syscall_ipc_connect,
    [50] = syscall_fb_present,
    [51] = syscall_shm_create_named,
    [52] = syscall_shm_open_named,
    [53] = syscall_shm_unlink_named,
    [54] = syscall_futex_wait,
    [55] = syscall_futex_wake,
    [56] = syscall_poll,
    [57] = syscall_ioctl,
    [58] = syscall_chdir,
    [59] = syscall_getcwd,
    [60] = syscall_uptime_ms,
    [61] = syscall_proc_list,
};

void syscall_handler(registers_t* regs) {
    __asm__ volatile("sti");
    uint32_t sys_num = regs->eax;
    if (sys_num >= (uint32_t)(sizeof(syscall_table) / sizeof(syscall_table[0]))) {
        regs->eax = (uint32_t)-1;
        return;
    }

    syscall_fn_t fn = syscall_table[sys_num];
    if (!fn) {
        regs->eax = (uint32_t)-1;
        return;
    }

    task_t* curr = proc_current();
    fn(regs, curr);
}
