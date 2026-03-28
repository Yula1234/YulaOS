/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2025 Yula1234 */

#include <kernel/waitq/poll_waitq.h>
#include <kernel/ipc/ipc_endpoint.h>
#include <kernel/uaccess/uaccess.h>
#include <kernel/tty/tty_bridge.h>
#include <kernel/futex/futex.h>
#include <kernel/input_focus.h>
#include <kernel/syscall.h>
#include <kernel/smp/cpu.h>
#include <kernel/smp/mb.h>

#include <drivers/input/keyboard.h>
#include <drivers/virtio/vgpu.h>
#include <drivers/video/fbdev.h>
#include <drivers/video/vga.h>

#include <fs/yulafs.h>
#include <fs/pipe.h>
#include <fs/vfs.h>

#include <hal/lock.h>
#include <hal/simd.h>
#include <hal/apic.h>

#include <mm/heap.h>
#include <mm/pmm.h>
#include <mm/vma.h>
#include <mm/shm.h>

#include <yos/ioctl.h>
#include <yos/proc.h>

#include <arch/i386/paging.h>

#include <lib/string.h>

#include "syscall.h"
#include "sched.h"
#include "proc.h"
#include "panic.h"

extern volatile uint32_t timer_ticks;

extern uint32_t* paging_get_dir(void); 

static int check_user_buffer(task_t* task, const void* buf, uint32_t size) {
    return uaccess_check_user_buffer(task, buf, size);
}

static uint8_t fb_present_fpu_tmp[MAX_CPUS][4096] __attribute__((aligned(64)));

__attribute__((always_inline))
static inline uint32_t irq_save_disable(void) {
    uint32_t flags;
    __asm__ volatile("pushfl; popl %0; cli" : "=r"(flags) : : "memory");
    return flags;
}

static int check_user_buffer_writable_present(task_t* task, void* buf, uint32_t size);

static int user_range_mappable(task_t* t, uintptr_t start, uintptr_t end_excl) {
    return uaccess_user_range_mappable(t, start, end_excl);
}

static int ensure_user_buffer_writable_mappable(task_t* task, void* buf, uint32_t size) {
    return uaccess_ensure_user_buffer_writable_mappable(task, buf, size);
}

static int check_user_buffer_present(task_t* task, const void* buf, uint32_t size) {
    return uaccess_check_user_buffer_present(task, buf, size);
}

static int check_user_buffer_writable_present(task_t* task, void* buf, uint32_t size) {
    return uaccess_check_user_buffer_writable_present(task, buf, size);
}

static int copy_user_str_bounded(task_t* task, char* dst, uint32_t dst_size, const char* user_src) {
    return uaccess_copy_user_str_bounded(task, dst, dst_size, user_src);
}

static constexpr uint32_t k_user_path_max = 256u;

static int copy_user_path(task_t* task, const char* user_path, char (&out)[k_user_path_max]) {
    if (!task || !user_path) {
        return -1;
    }

    if (copy_user_str_bounded(task, out, k_user_path_max, user_path) != 0) {
        return -1;
    }

    return 0;
}

static inline uint32_t align_down_4k_u32(uint32_t v) {
    return v & ~0xFFFu;
}

static inline uint32_t align_up_4k_u32(uint32_t v) {
    return (v + 0xFFFu) & ~0xFFFu;
}

static inline int ranges_overlap_u32(uint32_t a_start, uint32_t a_end, uint32_t b_start, uint32_t b_end) {
    return (a_start < b_end) && (b_start < a_end);
}

typedef struct {
    uint32_t type;
    uint32_t size;
    uint32_t inode;
    uint32_t flags;
    uint32_t created_at;
    uint32_t modified_at;
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

typedef void (*syscall_fn_t)(registers_t* regs, task_t* curr);

static void syscall_exit(registers_t* regs, task_t* curr) {
    curr->exit_status = (int)regs->ebx;
    proc_kill(curr);
    sched_yield();
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

    if (entry < 0x40000000u || entry >= 0xC0000000u) {
        regs->eax = (uint32_t)-1;
        return;
    }

    if (stack_bottom < 0x40000000u || stack_top > 0xC0000000u) {
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
    const char* user_path = (const char*)regs->ebx;
    const int flags = (int)regs->ecx;

    char path[k_user_path_max];
    if (copy_user_path(curr, user_path, path) != 0) {
        regs->eax = (uint32_t)-1;
        return;
    }

    regs->eax = (uint32_t)vfs_open(path, flags);
}

static void syscall_read(registers_t* regs, [[maybe_unused]] task_t* curr) {
    int res = vfs_read((int)regs->ebx, (void*)regs->ecx, (uint32_t)regs->edx);
    regs->eax = (uint32_t)res;
}

static void syscall_write(registers_t* regs, [[maybe_unused]] task_t* curr) {
    int res = vfs_write((int)regs->ebx, (const void*)regs->ecx, (uint32_t)regs->edx);
    regs->eax = (uint32_t)res;
}

static void syscall_close(registers_t* regs, task_t* curr) {
    (void)curr;
    regs->eax = (uint32_t)vfs_close((int)regs->ebx);
}

static void syscall_sleep(registers_t* regs, task_t* curr) {
    uint32_t ms = regs->ebx;
    uint32_t target = timer_ticks + (uint32_t)(((uint64_t)ms * KERNEL_TIMER_HZ) / 1000ull);
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

        if (vma_find_overlap(curr->mem, chk_start, chk_end_excl)) {
            regs->eax = (uint32_t)-1;
            return;
        }
    }

    if (incr < 0) {
        uint32_t start_free = (new_brk + 0xFFFu) & ~0xFFFu;
        uint32_t end_free = (old_brk + 0xFFFu) & ~0xFFFu;

        struct SbrkUnmapCtx {
            proc_mem_t* mem = nullptr;
        } ctx{curr->mem};

        auto visitor = [](uint32_t /*virt*/, uint32_t pte, void* vctx) -> int {
            if ((pte & 4u) == 0u) {
                return 0;
            }

            auto* ctxp = static_cast<SbrkUnmapCtx*>(vctx);
            if (!ctxp || !ctxp->mem) {
                return 0;
            }

            uint32_t phys = pte & ~0xFFFu;

            if (ctxp->mem->mem_pages > 0u) {
                ctxp->mem->mem_pages--;
            }

            if (phys != 0u && (pte & 0x200u) == 0u) {
                pmm_free_block((void*)phys);
            }

            return 1;
        };

        paging_unmap_range_ex(curr->mem->page_dir, start_free, end_free, visitor, &ctx);
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
        proc_task_put(t);
        sched_yield();
        regs->eax = 0;
        return;
    }

    if (t->mem && t->mem->page_dir) {
        __sync_fetch_and_or(&t->pending_signals, 1u << SIGTERM);
        proc_wake(t);
        regs->eax = 0;
    } else {
        regs->eax = (uint32_t)-1;
    }

    proc_task_put(t);
}

static void syscall_usleep(registers_t* regs, task_t* curr) {
    (void)curr;
    proc_usleep(regs->ebx);
}

static void syscall_get_mem_stats(registers_t* regs, task_t* curr) {
    uint32_t* u_ptr = (uint32_t*)regs->ebx;
    uint32_t* f_ptr = (uint32_t*)regs->ecx;

    if (!ensure_user_buffer_writable_mappable(curr, u_ptr, 4u)
        || !ensure_user_buffer_writable_mappable(curr, f_ptr, 4u)) {
        regs->eax = (uint32_t)-1;
        return;
    }

    const uint32_t used = pmm_get_used_blocks() * 4u;
    const uint32_t free = pmm_get_free_blocks() * 4u;

    if (uaccess_copy_to_user(u_ptr, &used, 4u) != 0
        || uaccess_copy_to_user(f_ptr, &free, 4u) != 0) {
        regs->eax = (uint32_t)-1;
        return;
    }

    regs->eax = 0;
}

static void syscall_mkdir(registers_t* regs, task_t* curr) {
    const char* user_path = (const char*)regs->ebx;

    char path[k_user_path_max];
    if (copy_user_path(curr, user_path, path) != 0) {
        regs->eax = (uint32_t)-1;
        return;
    }

    regs->eax = (uint32_t)vfs_mkdir(path);
}

static void syscall_mkdirat(registers_t* regs, task_t* curr) {
    int dirfd = (int)regs->ebx;
    const char* user_path = (const char*)regs->ecx;

    char path[k_user_path_max];
    if (copy_user_path(curr, user_path, path) != 0) {
        regs->eax = (uint32_t)-1;
        return;
    }

    regs->eax = (uint32_t)vfs_mkdirat(dirfd, path);
}

static void syscall_unlink(registers_t* regs, task_t* curr) {
    const char* user_path = (const char*)regs->ebx;

    char path[k_user_path_max];
    if (copy_user_path(curr, user_path, path) != 0) {
        regs->eax = (uint32_t)-1;
        return;
    }

    regs->eax = (uint32_t)vfs_unlink(path);
}

static void syscall_unlinkat(registers_t* regs, task_t* curr) {
    int dirfd = (int)regs->ebx;
    const char* user_path = (const char*)regs->ecx;

    char path[k_user_path_max];
    if (copy_user_path(curr, user_path, path) != 0) {
        regs->eax = (uint32_t)-1;
        return;
    }

    regs->eax = (uint32_t)vfs_unlinkat(dirfd, path);
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

static void syscall_set_term_mode(registers_t* regs, task_t* curr) {
    int mode = (int)regs->ebx;
    curr->term_mode = (mode == 1) ? 1 : 0;
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

    file_desc_t* nf = proc_fd_get(curr, newfd);
    if (nf) {
        file_desc_release(nf);
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
            if (!d->node->ops || !d->node->ops->get_phys_page) {
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
            if (!d->node->ops || !d->node->ops->read) {
                file_desc_release(d);
                regs->eax = 0;
                return;
            }
        }
    }

    if (!curr->mem) {
        file_desc_release(d);
        regs->eax = 0;
        return;
    }

    uint32_t vaddr = 0;
    if (!vma_alloc_slot(curr->mem, size, &vaddr)) {
        file_desc_release(d);
        regs->eax = 0;
        return;
    }

    uint32_t file_size = (d->node->size < size) ? d->node->size : size;

    vma_region_t* region = vma_create(curr->mem, vaddr, size, d->node, 0u, file_size, map_kind);
    if (!region) {
        file_desc_release(d);
        regs->eax = 0;
        return;
    }

    file_desc_release(d);

    regs->eax = vaddr;
}

static void syscall_munmap(registers_t* regs, task_t* curr) {
    uint32_t vaddr = regs->ebx;
    uint32_t len = regs->ecx;

    if (!curr->mem || !curr->mem->page_dir) {
        regs->eax = (uint32_t)-1;
        return;
    }

    int result = vma_remove(curr->mem, vaddr, len);

    regs->eax = (uint32_t)result;
}

static void syscall_stat(registers_t* regs, task_t* curr) {
    const char* user_path = (const char*)regs->ebx;
    user_stat_t* u_stat = (user_stat_t*)regs->ecx;

    char path[k_user_path_max];
    if (copy_user_path(curr, user_path, path) != 0
        || !check_user_buffer_writable_present(curr, u_stat, sizeof(*u_stat))) {
        regs->eax = (uint32_t)-1;
        return;
    }

    vfs_stat_t st;
    if (vfs_stat_path(path, &st) != 0) {
        regs->eax = (uint32_t)-1;
        return;
    }

    u_stat->type = st.type;
    u_stat->size = st.size;
    u_stat->inode = st.inode;
    u_stat->flags = st.flags;
    u_stat->created_at = st.created_at;
    u_stat->modified_at = st.modified_at;
    regs->eax = 0;
}

static void syscall_statat(registers_t* regs, task_t* curr) {
    int dirfd = (int)regs->ebx;
    const char* user_path = (const char*)regs->ecx;
    user_stat_t* u_stat = (user_stat_t*)regs->edx;

    char path[k_user_path_max];
    if (copy_user_path(curr, user_path, path) != 0
        || !check_user_buffer_writable_present(curr, u_stat, sizeof(*u_stat))) {
        regs->eax = (uint32_t)-1;
        return;
    }

    vfs_stat_t st;
    if (vfs_statat_path(dirfd, path, &st) != 0) {
        regs->eax = (uint32_t)-1;
        return;
    }

    u_stat->type = st.type;
    u_stat->size = st.size;
    u_stat->inode = st.inode;
    u_stat->flags = st.flags;
    u_stat->created_at = st.created_at;
    u_stat->modified_at = st.modified_at;
    regs->eax = 0;
}

static void syscall_get_fs_info(registers_t* regs, task_t* curr) {
    user_fs_info_t* u_info = (user_fs_info_t*)regs->ebx;

    if (!check_user_buffer(curr, u_info, sizeof(user_fs_info_t))) {
        regs->eax = (uint32_t)-1;
        return;
    }

    uint32_t total_blocks = 0;
    uint32_t free_blocks = 0;
    uint32_t block_size = 0;

    if (vfs_get_fs_info(&total_blocks, &free_blocks, &block_size) != 0) {
        regs->eax = (uint32_t)-1;
        return;
    }

    u_info->total_blocks = total_blocks;
    u_info->free_blocks = free_blocks;
    u_info->block_size = block_size;

    regs->eax = 0;
}

static void syscall_rename(registers_t* regs, task_t* curr) {
    const char* user_oldp = (const char*)regs->ebx;
    const char* user_newp = (const char*)regs->ecx;

    char oldp[k_user_path_max];
    char newp[k_user_path_max];
    if (copy_user_path(curr, user_oldp, oldp) != 0 || copy_user_path(curr, user_newp, newp) != 0) {
        regs->eax = (uint32_t)-1;
        return;
    }

    regs->eax = (uint32_t)vfs_rename(oldp, newp);
}

static void syscall_renameat(registers_t* regs, task_t* curr) {
    int old_dirfd = (int)regs->ebx;
    const char* user_oldp = (const char*)regs->ecx;
    int new_dirfd = (int)regs->edx;
    const char* user_newp = (const char*)regs->esi;

    char oldp[k_user_path_max];
    char newp[k_user_path_max];
    if (copy_user_path(curr, user_oldp, oldp) != 0 || copy_user_path(curr, user_newp, newp) != 0) {
        regs->eax = (uint32_t)-1;
        return;
    }

    regs->eax = (uint32_t)vfs_renameat(old_dirfd, oldp, new_dirfd, newp);
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

static void syscall_openat(registers_t* regs, task_t* curr) {
    int dirfd = (int)regs->ebx;
    const char* user_path = (const char*)regs->ecx;
    int flags = (int)regs->edx;

    char path[k_user_path_max];
    if (copy_user_path(curr, user_path, path) != 0) {
        regs->eax = (uint32_t)-1;
        return;
    }

    regs->eax = (uint32_t)vfs_openat(dirfd, path, flags);
}

static void syscall_fstatat(registers_t* regs, task_t* curr) {
    int dirfd = (int)regs->ebx;
    const char* user_name = (const char*)regs->ecx;
    user_stat_t* u_stat = (user_stat_t*)regs->edx;

    char name[k_user_path_max];
    if (copy_user_path(curr, user_name, name) != 0
        || !check_user_buffer_writable_present(curr, u_stat, sizeof(*u_stat))) {
        regs->eax = (uint32_t)-1;
        return;
    }

    regs->eax = (uint32_t)vfs_fstatat(dirfd, name, u_stat);
}

static int fb_get_mapping_params(uint32_t* out_fb_phys, uint32_t* out_size_bytes, uint32_t* out_page_off) {
    if (!out_fb_phys || !out_size_bytes || !out_page_off) {
        return 0;
    }

    *out_fb_phys = 0u;
    *out_size_bytes = 0u;
    *out_page_off = 0u;

    if (virtio_gpu_is_active()) {
        const virtio_gpu_fb_t* vfb = virtio_gpu_get_fb();
        if (!vfb || vfb->fb_phys == 0u || vfb->width == 0u || vfb->height == 0u || vfb->pitch == 0u) {
            return 0;
        }

        const uint64_t min_pitch64 = (uint64_t)vfb->width * 4ull;
        if (min_pitch64 > 0xFFFFFFFFull || vfb->pitch < (uint32_t)min_pitch64) {
            return 0;
        }

        const uint64_t size64 = (uint64_t)vfb->pitch * (uint64_t)vfb->height;
        if (size64 == 0u || size64 > 0xFFFFFFFFull || vfb->size_bytes < (uint32_t)size64) {
            return 0;
        }

        *out_fb_phys = vfb->fb_phys;
        *out_size_bytes = (uint32_t)size64;
    } else {
        if (!fb_ptr || fb_pitch == 0u || fb_width == 0u || fb_height == 0u) {
            return 0;
        }

        const uint64_t size64 = (uint64_t)fb_pitch * (uint64_t)fb_height;
        if (size64 == 0u || size64 > 0xFFFFFFFFull) {
            return 0;
        }

        *out_fb_phys = (uint32_t)fb_ptr;
        *out_size_bytes = (uint32_t)size64;
    }

    *out_page_off = (*out_fb_phys) & 0xFFFu;
    return 1;
}

static uint32_t fb_expected_user_ptr(void) {
    const uint32_t user_vaddr_start = 0xB1000000u;

    uint32_t fb_phys = 0u;
    uint32_t size_bytes = 0u;
    uint32_t page_off = 0u;

    if (!fb_get_mapping_params(&fb_phys, &size_bytes, &page_off)) {
        return 0u;
    }

    (void)fb_phys;
    (void)size_bytes;

    return user_vaddr_start + page_off;
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

    if (uaccess_copy_to_user(out, &c, 1u) != 0) {
        regs->eax = (uint32_t)-1;
        return;
    }

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

    ipc_connect_commit(pending);

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

    regs->eax = (uint32_t)futex_wait(key, uaddr, expected);
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

    regs->eax = (uint32_t)futex_wake(key, max_wake);
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
    uint64_t ms64 = ((uint64_t)timer_ticks * 1000ull) / (uint64_t)KERNEL_TIMER_HZ;
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

    if (!ensure_user_buffer_writable_mappable(curr, (void*)u_buf, bytes)) {
        regs->eax = (uint32_t)-1;
        return;
    }

    yos_proc_info_t* k_buf = (yos_proc_info_t*)kmalloc(bytes);
    if (!k_buf) {
        regs->eax = (uint32_t)-1;
        return;
    }

    const uint32_t count = proc_list_snapshot(k_buf, cap);
    const uint32_t out_bytes = count * (uint32_t)sizeof(*u_buf);

    if (out_bytes > 0u && uaccess_copy_to_user(u_buf, k_buf, out_bytes) != 0) {
        kfree(k_buf);
        regs->eax = (uint32_t)-1;
        return;
    }

    kfree(k_buf);
    regs->eax = count;
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

    if (!ensure_user_buffer_writable_mappable(curr, u_buf, size)) {
        regs->eax = (uint32_t)-1;
        return;
    }

    yfs_ino_t cur = (yfs_ino_t)(curr->cwd_inode ? curr->cwd_inode : 1u);

    char* k_buf = (char*)kmalloc(size);
    if (!k_buf) {
        regs->eax = (uint32_t)-1;
        return;
    }

    int len = yulafs_inode_to_path(cur, k_buf, size);
    if (len < 0) {
        kfree(k_buf);
        regs->eax = (uint32_t)-1;
        return;
    }

    const uint32_t out_bytes = (uint32_t)len + 1u;
    if (out_bytes > size) {
        kfree(k_buf);
        regs->eax = (uint32_t)-1;
        return;
    }

    if (uaccess_copy_to_user(u_buf, k_buf, out_bytes) != 0) {
        kfree(k_buf);
        regs->eax = (uint32_t)-1;
        return;
    }

    kfree(k_buf);

    regs->eax = (uint32_t)len;
}

static void syscall_setsid(registers_t* regs, task_t* curr) {
    regs->eax = (uint32_t)proc_setsid(curr);
}

static void syscall_setpgid(registers_t* regs, task_t* curr) {
    uint32_t arg0 = regs->ebx;
    uint32_t arg1 = regs->ecx;

    if (arg1 == 0u) {
        regs->eax = (uint32_t)proc_setpgid(curr, arg0);
        return;
    }

    uint32_t pid = arg0;
    uint32_t pgid = arg1;

    if (pid == 0u || pgid == 0u) {
        regs->eax = (uint32_t)-1;
        return;
    }

    task_t* target = proc_find_by_pid(pid);
    if (!target) {
        regs->eax = (uint32_t)-1;
        return;
    }

    if (target->sid != curr->sid) {
        proc_task_put(target);
        regs->eax = (uint32_t)-1;
        return;
    }

    if (target != curr && target->parent_pid != curr->pid) {
        proc_task_put(target);
        regs->eax = (uint32_t)-1;
        return;
    }

    regs->eax = (uint32_t)proc_setpgid(target, pgid);
    proc_task_put(target);
}

static void syscall_getpgrp(registers_t* regs, task_t* curr) {
    regs->eax = (uint32_t)proc_getpgrp(curr);
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
        const uint32_t fb_user_ptr = fb_expected_user_ptr();
        const int src_is_mapped_fb = (fb_user_ptr != 0u)
            && ((uintptr_t)req.src == (uintptr_t)fb_user_ptr)
            && (req.src_stride == dst_pitch);
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
            const uint32_t fb_user_ptr = fb_expected_user_ptr();
            const int flush_only = (fb_user_ptr != 0u)
                && ((uintptr_t)req.src == (uintptr_t)fb_user_ptr)
                && (req.src_stride == dst_pitch);

            if (!flush_only) {
                uint8_t* dst_base = (uint8_t*)dst_ptr;
                for (int y = y1; y < y2; y++) {
                    uint64_t src_row_off = (uint64_t)(uint32_t)y * (uint64_t)req.src_stride + (uint64_t)(uint32_t)x1 * 4ull;
                    uint64_t src_row_addr = (uint64_t)(uintptr_t)src_base + src_row_off;
                    uint64_t src_row_end_excl = src_row_addr + (uint64_t)row_bytes;

                    if (src_row_end_excl < src_row_addr || src_row_addr < 0x40000000ull || src_row_end_excl > 0xC0000000ull) {
                        regs->eax = (uint32_t)-1;
                        return;
                    }

                    if (!user_range_mappable(curr, (uintptr_t)src_row_addr, (uintptr_t)src_row_end_excl)) {
                        regs->eax = (uint32_t)-1;
                        return;
                    }

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

                if (row_end_excl < row_addr || row_addr < 0x40000000ull || row_end_excl > 0xC0000000ull) {
                    regs->eax = (uint32_t)-1;
                    return;
                }

                if (!user_range_mappable(curr, (uintptr_t)row_addr, (uintptr_t)row_end_excl)) {
                    regs->eax = (uint32_t)-1;
                    return;
                }
            }

            cpu_t* cpu = cpu_current();
            if (!cpu || cpu->index < 0 || cpu->index >= MAX_CPUS) {
                regs->eax = (uint32_t)-1;
                return;
            }

            uint8_t* fpu_tmp = &fb_present_fpu_tmp[cpu->index][0];
            uint32_t irq_flags = irq_save_disable();
            fpu_save(fpu_tmp);

            vga_present_rect(req.src, req.src_stride, x1, y1, x2 - x1, y2 - y1);

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

    constexpr uint32_t POLL_STACK_FAST_PATH = 8;

    pollfd_t stack_fds[POLL_STACK_FAST_PATH];
    poll_waiter_t stack_waiters[POLL_STACK_FAST_PATH];
    file_desc_t* stack_descs[POLL_STACK_FAST_PATH];

    pollfd_t* k_fds = 0;
    poll_waiter_t* waiters = 0;
    file_desc_t** active_descs = 0;

    bool use_heap = (nfds > POLL_STACK_FAST_PATH);

    if (nfds > 0) {
        if (use_heap) {
            k_fds = (pollfd_t*)kmalloc(bytes);
            waiters = (poll_waiter_t*)kmalloc((uint32_t)sizeof(poll_waiter_t) * nfds);
            active_descs = (file_desc_t**)kmalloc((uint32_t)sizeof(file_desc_t*) * nfds);
            if (!k_fds || !waiters || !active_descs) {
                if (k_fds) kfree(k_fds);
                if (waiters) kfree(waiters);
                if (active_descs) kfree(active_descs);
                regs->eax = (uint32_t)-1;
                return;
            }
            memset(active_descs, 0, (uint32_t)sizeof(file_desc_t*) * nfds);
            memset(waiters, 0, (uint32_t)sizeof(poll_waiter_t) * nfds);
        } else {
            k_fds = stack_fds;
            waiters = stack_waiters;
            active_descs = stack_descs;
            memset(active_descs, 0, nfds * sizeof(file_desc_t*));
            memset(waiters, 0, nfds * sizeof(poll_waiter_t));
        }

        if (uaccess_copy_from_user(k_fds, u_fds, bytes) != 0) {
            if (use_heap) {
                kfree(k_fds);
                kfree(waiters);
                kfree(active_descs);
            }
            regs->eax = (uint32_t)-1;
            return;
        }
    }

    uint32_t end_tick = 0;
    int have_deadline = 0;
    if (timeout_ms > 0) {
        uint64_t t = (uint64_t)timer_ticks + (uint64_t)((uint32_t)timeout_ms) * KERNEL_TIMER_HZ / 1000ull;
        if (t > 0xFFFFFFFFull) t = 0xFFFFFFFFull;
        end_tick = (uint32_t)t;
        have_deadline = 1;
    }

    const auto unblock_curr = [have_deadline](task_t* t) {
        if (!t) {
            return;
        }

        if (have_deadline) {
            proc_sleep_remove(t);
        }

        (void)proc_change_state(t, TASK_RUNNABLE);
    };

    int result = 0;

    for (;;) {
        if (waiters) {
            for (uint32_t i = 0; i < nfds; i++) {
                poll_waitq_unregister(&waiters[i]);

                if (active_descs && active_descs[i]) {
                    file_desc_release(active_descs[i]);
                    active_descs[i] = 0;
                }

                memset(&waiters[i], 0, sizeof(poll_waiter_t));
            }
        }

        if (curr->pending_signals != 0) {
            unblock_curr(curr);
            result = -2;
            goto out;
        }

        if (nfds == 0) {
            if (timeout_ms == 0) {
                unblock_curr(curr);
                goto out;
            }
            if (have_deadline && timer_ticks >= end_tick) {
                unblock_curr(curr);
                goto out;
            }

            if (have_deadline) {
                proc_sleep_add(curr, end_tick);
            } else {
                if (proc_change_state(curr, TASK_WAITING) != 0) {
                    unblock_curr(curr);
                    result = -2;
                    goto out;
                }
                sched_yield();
            }
            continue;
        }

        for (uint32_t i = 0; i < nfds; i++) {
            pollfd_t* p = &k_fds[i];

            if (p->revents != 0) continue;

            int32_t fd = p->fd;
            int16_t ev = p->events;

            if (fd < 0) continue;
            if ((ev & (VFS_POLLIN | VFS_POLLOUT)) == 0) continue;

            file_desc_t* d = proc_fd_get(curr, fd);
            if (!d || !d->node) {
                if (d) {
                    file_desc_release(d);
                }
                continue;
            }

            if (active_descs) {
                active_descs[i] = d;
            }

            vfs_node_t* node = d->node;

            if (node->ops && node->ops->poll_register) {
                (void)node->ops->poll_register(node, &waiters[i], curr);
            }

            if (!active_descs) {
                file_desc_release(d);
            }
        }

        if (proc_change_state(curr, TASK_WAITING) != 0) {
            unblock_curr(curr);
            result = -2;
            goto out;
        }
        smp_mb();

        int ready = 0;
        for (uint32_t i = 0; i < nfds; i++) {
            pollfd_t* p = &k_fds[i];

            int32_t fd = p->fd;
            int16_t ev = p->events;
            int16_t rev = 0;

            if (fd < 0) {
                rev = VFS_POLLNVAL;
            } else {
                file_desc_t* d = proc_fd_get(curr, fd);
                if (!d || !d->node) {
                    rev = VFS_POLLNVAL;
                } else {
                    vfs_node_t* node = d->node;

                    if (node->ops && node->ops->poll_status) {
                        rev = (int16_t)node->ops->poll_status(node, ev);
                    }
                }

                file_desc_release(d);
            }

            p->revents = rev;
            if (rev != 0) ready++;
        }

        if (ready > 0) {
            unblock_curr(curr);
            result = ready;
            goto out;
        }

        if (timeout_ms == 0) {
            unblock_curr(curr);
            goto out;
        }

        if (have_deadline && timer_ticks >= end_tick) {
            unblock_curr(curr);
            goto out;
        }

        if (have_deadline) {
            if (curr->state == TASK_WAITING) {
                proc_sleep_add(curr, end_tick);
            }
        } else {
            if (curr->state == TASK_WAITING) {
                sched_yield();
            }
        }
    }

out:
    if (waiters) {
        for (uint32_t i = 0; i < nfds; i++) {
            poll_waitq_unregister(&waiters[i]);

            if (active_descs && active_descs[i]) {
                file_desc_release(active_descs[i]);
            }
        }
    }

    if (result < 0 && k_fds) {
        for (uint32_t i = 0; i < nfds; i++) {
            k_fds[i].revents = 0;
        }
    }

    if (k_fds && bytes) {
        if (uaccess_copy_to_user(u_fds, k_fds, bytes) != 0) {
            result = -1;
        }
    }

    if (use_heap) {
        if (k_fds) kfree(k_fds);
        if (waiters) kfree(waiters);
        if (active_descs) kfree(active_descs);
    }

    regs->eax = (uint32_t)result;
}

static const syscall_fn_t syscall_table[] = {
    [0] = syscall_exit,
    [1] = syscall_getpid,
    [2] = syscall_open,
    [3] = syscall_read,
    [4] = syscall_write,
    [5] = syscall_close,
    [6] = syscall_sleep,
    [7] = syscall_sbrk,
    [8] = syscall_kill,
    [9] = syscall_usleep,
    [10] = syscall_get_mem_stats,
    [11] = syscall_mkdir,
    [12] = syscall_unlink,
    [13] = syscall_renameat,
    [14] = syscall_reboot,
    [15] = syscall_signal,
    [16] = syscall_sigreturn,
    [17] = syscall_clone,
    [18] = syscall_set_term_mode,
    [19] = syscall_pipe,
    [20] = syscall_dup2,
    [21] = syscall_mmap,
    [22] = syscall_munmap,
    [23] = syscall_stat,
    [24] = syscall_get_fs_info,
    [25] = syscall_rename,
    [26] = syscall_spawn_process,
    [27] = syscall_waitpid,
    [28] = syscall_getdents,
    [29] = syscall_fstatat,
    [30] = syscall_shm_create,
    [31] = syscall_pipe_try_read,
    [32] = syscall_pipe_try_write,
    [33] = syscall_kbd_try_read,
    [34] = syscall_ipc_listen,
    [35] = syscall_ipc_accept,
    [36] = syscall_ipc_connect,
    [37] = syscall_fb_present,
    [38] = syscall_shm_create_named,
    [39] = syscall_shm_open_named,
    [40] = syscall_shm_unlink_named,
    [41] = syscall_futex_wait,
    [42] = syscall_futex_wake,
    [43] = syscall_poll,
    [44] = syscall_ioctl,
    [45] = syscall_chdir,
    [46] = syscall_getcwd,
    [47] = syscall_uptime_ms,
    [48] = syscall_proc_list,
    [49] = syscall_setsid,
    [50] = syscall_setpgid,
    [51] = syscall_getpgrp,
    [52] = syscall_openat,
    [53] = syscall_mkdirat,
    [54] = syscall_unlinkat,
    [55] = syscall_statat,
};

extern "C" void syscall_handler(registers_t* regs) {
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
