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
#include <hal/apic.h>
#include <drivers/fbdev.h>
#include <kernel/input_focus.h>

#include <lib/hash_map.h>
#include <lib/cpp/dlist.h>
#include <lib/cpp/lock_guard.h>
#include <lib/cpp/rbtree.h>
#include <lib/cpp/unique_ptr.h>

#include "sched.h"
#include "proc.h"
#include "poll_waitq.h"
#include "elf.h"
#include "cpu.h"

#define PID_MAP_BUCKETS 256

#define EI_CLASS 4
#define EI_DATA 5
#define EI_VERSION 6
#define ELFCLASS32 1
#define ELFDATA2LSB 1
#define EV_CURRENT 1
#define ET_EXEC 2
#define EM_386 3
#define PT_LOAD 1

namespace proc {
namespace detail {

static constexpr uint32_t irq_if_mask = 0x200u;

static constexpr uint32_t user_elf_min_vaddr = 0x08000000u;
static constexpr uint32_t user_elf_max_vaddr = 0xB0000000u;

static constexpr uint32_t user_stack_addr_min = 0x08000000u;
static constexpr uint32_t user_stack_addr_max = 0xC0000000u;

static constexpr uint32_t default_mmap_top = 0x80001000u;

static constexpr uint32_t max_elf_phdrs = 64u;

static constexpr int max_user_stack_argc = 16;

static constexpr uint32_t user_stack_size_bytes = 4u * 1024u * 1024u;
static constexpr uint32_t user_stack_top_limit = 0xB0400000u;

static constexpr uint32_t fd_table_cap_limit = 0x7FFFFFFFu;

static constexpr uint32_t page_size = 4096u;
static constexpr uint32_t page_mask = page_size - 1u;

static constexpr uint32_t stack_align_bytes = 16u;
static constexpr uint32_t stack_align_mask = stack_align_bytes - 1u;
static constexpr uint32_t user_stack_min_slack = 8u;

static constexpr uint32_t user_ds_selector = 0x23u;
static constexpr uint32_t user_cs_selector = 0x1Bu;
static constexpr uint32_t user_initial_eflags = 0x202u;

static constexpr uint32_t heap_mmap_bump = 0x100000u;

using AllTasksList = kernel::CDBLinkedList<task_t, &task_t::all_tasks_node>;

static AllTasksList all_tasks;

static uint32_t total_tasks = 0;
static uint32_t next_pid = 1;

static kernel::SpinLock proc_lock;

struct SleepKey {
    uint32_t wake_tick = 0;
    uintptr_t tie = 0;
};

struct SleepKeyOfValue {
    const SleepKey operator()(const task_t& t) const noexcept {
        return SleepKey{
            t.wake_tick,
            reinterpret_cast<uintptr_t>(&t),
        };
    }
};

struct SleepKeyLess {
    bool operator()(const SleepKey& a, const SleepKey& b) const noexcept {
        if (a.wake_tick != b.wake_tick) {
            return a.wake_tick < b.wake_tick;
        }

        return a.tie < b.tie;
    }
};

using SleepHook = kernel::detail::RbMemberHook<task_t, offsetof(task_t, sleep_rb)>;
using SleepingTree = kernel::IntrusiveRbTree<task_t, SleepHook, SleepKey, SleepKeyOfValue, SleepKeyLess>;

static SleepingTree sleeping_tree;
static kernel::SpinLock sleep_lock;

static dlist_head_t zombie_list;
static kernel::SpinLock zombie_lock;

static HashMap<uint32_t, task_t*, PID_MAP_BUCKETS> pid_map;

static uint8_t* initial_fpu_state = 0;
static uint32_t initial_fpu_state_size = 0;

class ScopedIrqDisable {
public:
    ScopedIrqDisable() {
        uint32_t flags;
        __asm__ volatile (
            "pushfl\n\t"
            "popl %0\n\t"
            "cli"
            : "=r"(flags)
            :
            : "memory"
        );

        flags_ = flags;
    }

    ScopedIrqDisable(const ScopedIrqDisable&) = delete;
    ScopedIrqDisable& operator=(const ScopedIrqDisable&) = delete;

    ScopedIrqDisable(ScopedIrqDisable&&) = delete;
    ScopedIrqDisable& operator=(ScopedIrqDisable&&) = delete;

    ~ScopedIrqDisable() {
        if ((flags_ & irq_if_mask) != 0u) {
            __asm__ volatile("sti");
        }
    }

private:
    uint32_t flags_ = 0u;
};

class ScopedPagingSwitch {
public:
    explicit ScopedPagingSwitch(uint32_t* dir)
        : prev_dir_(paging_get_dir()) {
        paging_switch(dir);
    }

    ScopedPagingSwitch(const ScopedPagingSwitch&) = delete;
    ScopedPagingSwitch& operator=(const ScopedPagingSwitch&) = delete;

    ScopedPagingSwitch(ScopedPagingSwitch&&) = delete;
    ScopedPagingSwitch& operator=(ScopedPagingSwitch&&) = delete;

    ~ScopedPagingSwitch() {
        paging_switch(prev_dir_);
    }

private:
    uint32_t* prev_dir_ = nullptr;
};

template<typename T>
struct KfreeDeleter {
    void operator()(T* ptr) const noexcept {
        if (ptr) {
            kfree(ptr);
        }
    }
};

struct VfsNodeReleaseDeleter {
    void operator()(vfs_node_t* node) const noexcept {
        if (node) {
            vfs_node_release(node);
        }
    }
};

class FileDescRef {
public:
    FileDescRef() = default;

    static FileDescRef adopt(file_desc_t* desc) noexcept {
        return FileDescRef(desc);
    }

    static FileDescRef from_borrowed(file_desc_t* desc) noexcept {
        if (!desc) {
            return {};
        }

        file_desc_retain(desc);
        return adopt(desc);
    }

    FileDescRef(const FileDescRef&) = delete;
    FileDescRef& operator=(const FileDescRef&) = delete;

    FileDescRef(FileDescRef&& other) noexcept
        : desc_(other.desc_) {
        other.desc_ = nullptr;
    }

    FileDescRef& operator=(FileDescRef&& other) noexcept {
        if (this == &other) {
            return *this;
        }

        reset();

        desc_ = other.desc_;
        other.desc_ = nullptr;

        return *this;
    }

    ~FileDescRef() {
        reset();
    }

    file_desc_t* get() const noexcept {
        return desc_;
    }

    explicit operator bool() const noexcept {
        return desc_ != nullptr;
    }

    file_desc_t* detach() noexcept {
        file_desc_t* out = desc_;
        desc_ = nullptr;
        return out;
    }

private:
    explicit FileDescRef(file_desc_t* desc) noexcept
        : desc_(desc) {
    }

    void reset() noexcept {
        if (!desc_) {
            return;
        }

        file_desc_release(desc_);
        desc_ = nullptr;
    }

    file_desc_t* desc_ = nullptr;
};

class FdTableRef {
public:
    FdTableRef() = default;

    static FdTableRef adopt(fd_table_t* table) noexcept {
        return FdTableRef(table);
    }

    static FdTableRef from_borrowed(fd_table_t* table) noexcept {
        if (!table) {
            return {};
        }

        proc_fd_table_retain(table);
        return adopt(table);
    }

    FdTableRef(const FdTableRef&) = delete;
    FdTableRef& operator=(const FdTableRef&) = delete;

    FdTableRef(FdTableRef&& other) noexcept
        : table_(other.table_) {
        other.table_ = nullptr;
    }

    FdTableRef& operator=(FdTableRef&& other) noexcept {
        if (this == &other) {
            return *this;
        }

        reset();

        table_ = other.table_;
        other.table_ = nullptr;

        return *this;
    }

    ~FdTableRef() {
        reset();
    }

    fd_table_t* get() const noexcept {
        return table_;
    }

    explicit operator bool() const noexcept {
        return table_ != nullptr;
    }

    fd_table_t* detach() noexcept {
        fd_table_t* out = table_;
        table_ = nullptr;
        return out;
    }

private:
    explicit FdTableRef(fd_table_t* table) noexcept
        : table_(table) {
    }

    void reset() noexcept {
        if (!table_) {
            return;
        }

        proc_fd_table_release(table_);
        table_ = nullptr;
    }

    fd_table_t* table_ = nullptr;
};

static void pid_map_insert(uint32_t pid, task_t* t) {
    if (!t) return;
    (void)pid_map.insert_or_assign(pid, t);
}

static void pid_map_remove(uint32_t pid) {
    (void)pid_map.remove(pid);
}

}
}

extern "C" void irq_return(void);
extern "C" void idle_task_func(void*);
extern volatile uint32_t timer_ticks;

uint32_t proc_list_snapshot(yos_proc_info_t* out, uint32_t cap) {
    if (!out || cap == 0) return 0;

    uint32_t count = 0;
    kernel::SpinLockSafeGuard guard(proc::detail::proc_lock);

    for (task_t& t : proc::detail::all_tasks) {
        if (count >= cap) {
            break;
        }

        if (t.state == TASK_UNUSED) {
            continue;
        }

        yos_proc_info_t* e = &out[count++];
        e->pid = t.pid;
        e->parent_pid = t.parent_pid;
        e->state = (uint32_t)t.state;
        e->priority = (uint32_t)t.priority;
        e->mem_pages = (t.mem) ? t.mem->mem_pages : 0;
        e->term_mode = (uint32_t)t.term_mode;
        strlcpy(e->name, t.name, sizeof(e->name));
    }
    return count;
}

void proc_init(void) {
    proc::detail::total_tasks = 0;
    proc::detail::next_pid = 1;

    spinlock_init(proc::detail::proc_lock.native_handle());

    proc::detail::all_tasks.clear_links_unsafe();

    proc::detail::sleeping_tree.clear();

    spinlock_init(proc::detail::sleep_lock.native_handle());

    dlist_init(&proc::detail::zombie_list);

    spinlock_init(proc::detail::zombie_lock.native_handle());


    proc::detail::initial_fpu_state_size = fpu_state_size();
    kernel::unique_ptr<uint8_t, proc::detail::KfreeDeleter<uint8_t>> fpu_state_guard(
        static_cast<uint8_t*>(kmalloc_a(proc::detail::initial_fpu_state_size))
    );
    if (!fpu_state_guard) {
        return;
    }

    __asm__ volatile("fninit");
    fpu_save(fpu_state_guard.get());
    proc::detail::initial_fpu_state = fpu_state_guard.release();
}

task_t* proc_current() { 
    cpu_t* cpu = cpu_current();
    return cpu->current_task; 
}

void proc_fd_table_init(task_t* t) {
    if (!t) return;

    fd_table_t* ft = static_cast<fd_table_t*>(kmalloc(sizeof(*ft)));
    if (!ft) {
        t->fd_table = 0;
        return;
    }

    memset(ft, 0, sizeof(*ft));
    ft->refs = 1;
    spinlock_init(&ft->lock);
    
    ft->max_fds = 32;
    ft->fds = static_cast<file_desc_t**>(kmalloc(sizeof(file_desc_t*) * ft->max_fds));
    if (!ft->fds) {
        kfree(ft);
        t->fd_table = 0;
        return;
    }
    memset(ft->fds, 0, sizeof(file_desc_t*) * ft->max_fds);
    
    ft->fd_next = 0;
    t->fd_table = ft;
}

void proc_fd_table_retain(fd_table_t* ft) {
    if (!ft) return;
    __sync_fetch_and_add(&ft->refs, 1);
}

void file_desc_retain(file_desc_t* d) {
    if (!d) return;
    __sync_fetch_and_add(&d->refs, 1);
}

void file_desc_release(file_desc_t* d) {
    if (!d) return;
    if (__sync_sub_and_fetch(&d->refs, 1) != 0) return;

    if (d->node) {
        vfs_node_release(d->node);
        d->node = 0;
    }

    kfree(d);
}

void proc_fd_table_release(fd_table_t* ft) {
    if (!ft) return;

    uint32_t old = __sync_fetch_and_sub(&ft->refs, 1);
    if (old == 0) {
        ft->refs = 0;
        return;
    }
    if (old > 1) return;

    if (ft->fds) {
        for (uint32_t i = 0; i < ft->max_fds; i++) {
            if (ft->fds[i]) {
                file_desc_release(ft->fds[i]);
                ft->fds[i] = 0;
            }
        }
        kfree(ft->fds);
        ft->fds = 0;
    }

    kfree(ft);
}

file_desc_t* proc_fd_get(task_t* t, int fd) {
    if (!t || fd < 0) return 0;
    fd_table_t* ft = t->fd_table;
    if (!ft) return 0;

    kernel::SpinLockNativeSafeGuard guard(ft->lock);
    
    file_desc_t* out = 0;
    if ((uint32_t)fd < ft->max_fds && ft->fds) {
        out = ft->fds[fd];
    }

    proc::detail::FileDescRef out_ref = proc::detail::FileDescRef::from_borrowed(out);
    return out_ref.detach();
}

static int fd_table_ensure_cap(fd_table_t* ft, uint32_t required_fd) {
    if (required_fd < ft->max_fds) return 0;
    
    uint32_t new_cap = ft->max_fds ? ft->max_fds : 32;
    while (new_cap <= required_fd) {
        if (new_cap >= proc::detail::fd_table_cap_limit) return -1; 
        new_cap *= 2;
    }
    
    file_desc_t** new_fds = static_cast<file_desc_t**>(kmalloc(sizeof(file_desc_t*) * new_cap));
    if (!new_fds) return -1;
    
    memset(new_fds, 0, sizeof(file_desc_t*) * new_cap);
    if (ft->fds) {
        memcpy(new_fds, ft->fds, sizeof(file_desc_t*) * ft->max_fds);
        kfree(ft->fds);
    }
    
    ft->fds = new_fds;
    ft->max_fds = new_cap;
    return 0;
}

int proc_fd_add_at(task_t* t, int fd, file_desc_t** out_desc) {
    if (out_desc) *out_desc = 0;
    if (!t || fd < 0) return -1;
    fd_table_t* ft = t->fd_table;
    if (!ft) return -1;

    kernel::SpinLockNativeSafeGuard guard(ft->lock);
    
    if (fd_table_ensure_cap(ft, (uint32_t)fd) != 0) {
        return -1;
    }

    if (ft->fds[fd]) {
        return -1;
    }

    file_desc_t* d = static_cast<file_desc_t*>(kmalloc(sizeof(*d)));
    if (!d) {
        return -1;
    }

    memset(d, 0, sizeof(*d));
    d->refs = 1;
    spinlock_init(&d->lock);

    ft->fds[fd] = d;
    if (fd >= ft->fd_next) ft->fd_next = fd + 1;
    
    if (out_desc) *out_desc = d;
    return fd;
}

int proc_fd_install_at(task_t* t, int fd, file_desc_t* desc) {
    if (!t || fd < 0 || !desc) return -1;
    fd_table_t* ft = t->fd_table;
    if (!ft) return -1;

    kernel::SpinLockNativeSafeGuard guard(ft->lock);
    
    if (fd_table_ensure_cap(ft, (uint32_t)fd) != 0) {
        return -1;
    }

    if (ft->fds[fd]) {
        return -1;
    }

    ft->fds[fd] = desc;
    file_desc_retain(desc);

    if (fd >= ft->fd_next) ft->fd_next = fd + 1;
    
    return fd;
}

int proc_fd_alloc(task_t* t, file_desc_t** out_desc) {
    if (out_desc) *out_desc = 0;
    if (!t) return -1;
    fd_table_t* ft = t->fd_table;
    if (!ft) return -1;

    int found = -1;

    {
        kernel::SpinLockNativeSafeGuard guard(ft->lock);

        int expected = ft->fd_next;
        if (expected < 0) expected = 0;

        uint32_t limit = ft->max_fds;
        uint32_t start = (uint32_t)expected;

        for (uint32_t i = start; i < limit; i++) {
            if (ft->fds[i] == 0) {
                found = (int)i;
                break;
            }
        }

        if (found == -1 && start > 0) {
            for (uint32_t i = 0; i < start; i++) {
                if (ft->fds[i] == 0) {
                    found = (int)i;
                    break;
                }
            }
        }

        if (found == -1) {
            found = (int)limit;
            if (fd_table_ensure_cap(ft, limit) != 0) {
                return -1;
            }
        }

        ft->fd_next = found + 1;
    }

    return proc_fd_add_at(t, found, out_desc);
}

int proc_fd_remove(task_t* t, int fd, file_desc_t** out_desc) {
    if (out_desc) *out_desc = 0;
    if (!t || fd < 0) return -1;
    fd_table_t* ft = t->fd_table;
    if (!ft) return -1;

    kernel::SpinLockNativeSafeGuard guard(ft->lock);
    
    if ((uint32_t)fd >= ft->max_fds || !ft->fds || !ft->fds[fd]) {
        return -1;
    }

    file_desc_t* d = ft->fds[fd];
    ft->fds[fd] = 0;

    if (fd < ft->fd_next) ft->fd_next = fd;

    if (out_desc) *out_desc = d;
    return 0;
}

static fd_table_t* proc_fd_table_clone(fd_table_t* src) {
    if (!src) return 0;

    fd_table_t* ft_raw = static_cast<fd_table_t*>(kmalloc(sizeof(*ft_raw)));
    if (!ft_raw) return 0;

    proc::detail::FdTableRef ft = proc::detail::FdTableRef::adopt(ft_raw);

    memset(ft.get(), 0, sizeof(*ft.get()));
    ft.get()->refs = 1;
    spinlock_init(&ft.get()->lock);

    kernel::SpinLockNativeSafeGuard guard(src->lock);
    
    ft.get()->max_fds = src->max_fds;
    ft.get()->fds = static_cast<file_desc_t**>(kmalloc(sizeof(file_desc_t*) * ft.get()->max_fds));
    if (!ft.get()->fds) {
        return 0;
    }
    memset(ft.get()->fds, 0, sizeof(file_desc_t*) * ft.get()->max_fds);
    
    ft.get()->fd_next = src->fd_next;

    for (uint32_t i = 0; i < ft.get()->max_fds; i++) {
        if (src->fds[i]) {
            ft.get()->fds[i] = src->fds[i];
            file_desc_retain(ft.get()->fds[i]);
        }
    }

    return ft.detach();
}

task_t* proc_find_by_pid(uint32_t pid) {
    task_t* t = 0;
    if (!proc::detail::pid_map.try_get(pid, t)) {
        return 0;
    }
    return t;
}

static proc_mem_t* proc_mem_create(uint32_t leader_pid) {
    kernel::unique_ptr<proc_mem_t, proc::detail::KfreeDeleter<proc_mem_t>> mem_guard(
        static_cast<proc_mem_t*>(kmalloc_a(sizeof(proc_mem_t)))
    );
    if (!mem_guard) return 0;

    proc_mem_t* mem = mem_guard.get();
    memset(mem, 0, sizeof(*mem));
    mem->leader_pid = leader_pid;
    mem->refcount = 1;
    mem->mmap_top = proc::detail::default_mmap_top;
    mem->page_dir = paging_clone_directory();
    if (!mem->page_dir) {
        return 0;
    }

    return mem_guard.release();
}

static void proc_mem_retain(proc_mem_t* mem) {
    if (!mem) return;
    __sync_fetch_and_add(&mem->refcount, 1);
}

static void proc_mem_release(proc_mem_t* mem) {
    if (!mem) return;

    uint32_t old = __sync_fetch_and_sub(&mem->refcount, 1);
    if (old == 0) {
        mem->refcount = 0;
        return;
    }
    if (old > 1) return;

    if (mem->leader_pid) {
        fb_release_by_pid(mem->leader_pid);
        if (input_focus_get_pid() == mem->leader_pid) {
            input_focus_set_pid(0);
        }
    }

    mmap_area_t* m = mem->mmap_list;
    while (m) {
        mmap_area_t* next = m->next;
        if (m->file) {
            vfs_node_release(m->file);
        }
        kfree(m);
        m = next;
    }
    mem->mmap_list = 0;

    if (mem->page_dir && mem->page_dir != kernel_page_directory) {
        for (int i = 0; i < 1024; i++) {
            uint32_t pde = mem->page_dir[i];

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
                        } else if (pte & 4) {
                            void* physical_page = (void*)(pte & ~0xFFF);
                            pmm_free_block(physical_page);
                        }
                    }
                }
                pmm_free_block(pt);
            }
        }
        pmm_free_block(mem->page_dir);
        mem->page_dir = 0;
    }

    mem->mem_pages = 0;
    mem->fbmap_pages = 0;
    mem->fbmap_user_ptr = 0;
    mem->fbmap_size_bytes = 0;
    mem->fbmap_is_virtio = 0;

    kfree(mem);
}

static task_t* alloc_task(void) {
    kernel::SpinLockSafeGuard guard(proc::detail::proc_lock);
    
    kernel::unique_ptr<task_t, proc::detail::KfreeDeleter<task_t>> t_guard(
        static_cast<task_t*>(kmalloc_a(sizeof(task_t)))
    );
    if (!t_guard) {
        return 0;
    }

    task_t* t = t_guard.get();
    memset(t, 0, sizeof(task_t));
    proc_fd_table_init(t);

    spinlock_init(&t->poll_lock);
    dlist_init(&t->poll_waiters);

    if (!proc::detail::initial_fpu_state) {
        return 0;
    }

    t->fpu_state_size = proc::detail::initial_fpu_state_size;
    kernel::unique_ptr<uint8_t, proc::detail::KfreeDeleter<uint8_t>> fpu_state_guard(
        static_cast<uint8_t*>(kmalloc_a(t->fpu_state_size))
    );
    if (!fpu_state_guard) {
        return 0;
    }

    memcpy(fpu_state_guard.get(), proc::detail::initial_fpu_state, t->fpu_state_size);
    t->fpu_state = fpu_state_guard.release();
    
    sem_init(&t->exit_sem, 0); 

    t->blocked_on_sem = 0;
    t->is_queued = 0;
    t->pid = proc::detail::next_pid++;
    t->state = TASK_RUNNABLE;
    t->cwd_inode = 1;
    t->term_mode = 0;
    t->assigned_cpu = -1;
    t->vruntime = 0;
    t->exec_start = 0;

    proc::detail::pid_map_insert(t->pid, t);

    proc::detail::all_tasks.push_back(*t);
    proc::detail::total_tasks++;

    return t_guard.release();
}

void proc_free_resources(task_t* t) {
    if (!t) return;

    proc_sleep_remove(t);
    poll_task_cleanup(t);

    if (t->fd_table) {
        proc_fd_table_release(t->fd_table);
        t->fd_table = 0;
    }

    if (t->mem) {
        proc_mem_release(t->mem);
        t->mem = 0;
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
    
    proc::detail::pid_map_remove(t->pid);

    t->pid = 0;
    memset(t->name, 0, sizeof(t->name));
    
    kernel::SpinLockSafeGuard guard(proc::detail::proc_lock);
    dlist_del(&t->all_tasks_node);
    if (proc::detail::total_tasks > 0) {
        proc::detail::total_tasks--;
    }
    kfree(t);
}

void proc_kill(task_t* t) {
    if (!t) return;
    
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

    {
        kernel::SpinLockSafeGuard guard(proc::detail::proc_lock);
        uint32_t pid_to_clean = (uint32_t)t->pid;

        for (task_t& child : proc::detail::all_tasks) {
            if (child.parent_pid != pid_to_clean) {
                continue;
            }

            if (child.state == TASK_UNUSED) {
                continue;
            }

            child.parent_pid = 0;
        }
    }

    sem_remove_task(t);

    poll_task_cleanup(t);

    proc_sleep_remove(t);

    sched_remove(t);

    t->state = TASK_ZOMBIE;

    {
        kernel::SpinLockSafeGuard guard(proc::detail::zombie_lock);
        dlist_add_tail(&t->zombie_node, &proc::detail::zombie_list);
    }

    sem_signal_all(&t->exit_sem);
}

static void kthread_trampoline(void) {
    task_t* t = proc_current();
    __asm__ volatile("sti");
    t->entry(t->arg);       
    
    t->state = TASK_ZOMBIE;
    sched_yield();        
    for (;;) cpu_hlt();   
}

static int proc_alloc_kstack(task_t* t);
static uint32_t* proc_kstack_top(task_t* t);

task_t* proc_spawn_kthread(const char* name, task_prio_t prio, void (*entry)(void*), void* arg) {
    task_t* t = alloc_task();
    if (!t) return 0;

    strlcpy(t->name, name ? name : "task", sizeof(t->name));
    t->entry = entry;
    t->arg = arg;
    t->mem = 0;
    t->priority = prio;

    if (!proc_alloc_kstack(t)) {
        proc_free_resources(t);
        return 0;
    }

    uint32_t* sp = proc_kstack_top(t);
    *--sp = 0;
    *--sp = 0;
    *--sp = 0;

    *--sp = (uint32_t)kthread_trampoline;
    *--sp = 0;
    *--sp = 0;
    *--sp = 0;
    *--sp = 0;
    t->esp = sp;

    sched_add(t);
    return t;
}

task_t* proc_get_list_head() {
    if (proc::detail::all_tasks.empty()) {
        return 0;
    }
    return &proc::detail::all_tasks.front();
}
uint32_t proc_task_count(void) { return proc::detail::total_tasks; }

task_t* proc_task_at(uint32_t idx) {
    kernel::SpinLockSafeGuard guard(proc::detail::proc_lock);

    uint32_t i = 0;
    for (task_t& curr : proc::detail::all_tasks) {
        if (i == idx) {
            return &curr;
        }
        i++;
    }
    return 0;
}

static int proc_alloc_kstack(task_t* t) {
    if (!t) return 0;

    t->kstack_size = KSTACK_SIZE;
    kernel::unique_ptr<uint8_t, proc::detail::KfreeDeleter<uint8_t>> kstack_guard(
        static_cast<uint8_t*>(kmalloc_a(t->kstack_size))
    );
    if (!kstack_guard) return 0;

    memset(kstack_guard.get(), 0, t->kstack_size);
    t->kstack = kstack_guard.release();
    return 1;
}

static uint32_t* proc_kstack_top(task_t* t) {
    if (!t || !t->kstack) return 0;

    uint32_t stack_top = (uint32_t)t->kstack + t->kstack_size;
    stack_top &= ~proc::detail::stack_align_mask;
    return (uint32_t*)stack_top;
}

static void proc_init_user_context(task_t* t, uint32_t user_eip, uint32_t user_esp) {
    if (!t || !t->kstack) return;

    uint32_t* sp = proc_kstack_top(t);
    *--sp = proc::detail::user_ds_selector;
    *--sp = user_esp;
    *--sp = proc::detail::user_initial_eflags;
    *--sp = proc::detail::user_cs_selector;
    *--sp = user_eip;

    *--sp = (uint32_t)irq_return;

    *--sp = 0;
    *--sp = 0;
    *--sp = 0;
    *--sp = 0;

    t->esp = sp;
}

static int proc_setup_thread_user_stack(task_t* t, uint32_t stack_bottom, uint32_t stack_top, uint32_t arg, uint32_t* out_user_esp) {
    if (!t || !t->mem || !t->mem->page_dir || !out_user_esp) return 0;

    uint32_t user_sp = stack_top & ~proc::detail::stack_align_mask;
    if (user_sp < stack_bottom + proc::detail::user_stack_min_slack) return 0;

    uint32_t sp = user_sp;

    {
        proc::detail::ScopedIrqDisable irq_guard;
        proc::detail::ScopedPagingSwitch paging_guard(t->mem->page_dir);

        sp -= 4u;
        *(uint32_t*)sp = arg;
        sp -= 4u;
        *(uint32_t*)sp = 0u;
    }

    *out_user_esp = sp;
    return 1;
}

static int proc_mem_has_mmap_overlap(proc_mem_t* mem, uint32_t start, uint32_t end_excl);
static int proc_mem_register_stack_region(proc_mem_t* mem, uint32_t stack_bottom, uint32_t stack_top);

task_t* proc_clone_thread(uint32_t entry, uint32_t arg, uint32_t stack_bottom, uint32_t stack_top) {
    task_t* parent = proc_current();
    if (!parent || !parent->mem || !parent->mem->page_dir) return 0;

    task_t* t = alloc_task();
    if (!t) return 0;

    strlcpy(t->name, parent->name, sizeof(t->name));
    t->parent_pid = parent->pid;
    t->cwd_inode = parent->cwd_inode;
    t->terminal = parent->terminal;
    t->term_mode = parent->term_mode;
    t->priority = parent->priority;
    t->stack_bottom = stack_bottom;
    t->stack_top = stack_top;

    t->mem = parent->mem;
    proc_mem_retain(t->mem);

    if (parent->fd_table) {
        proc_fd_table_release(t->fd_table);
        t->fd_table = parent->fd_table;
        proc_fd_table_retain(t->fd_table);
    }

    if (!proc_alloc_kstack(t)) {
        proc_free_resources(t);
        return 0;
    }

    uint32_t user_esp = 0;
    if (!proc_setup_thread_user_stack(t, stack_bottom, stack_top, arg, &user_esp)) {
        proc_free_resources(t);
        return 0;
    }

    if (t->mem && t->mem->heap_start < t->mem->prog_break) {
        uint32_t hs = t->mem->heap_start;
        uint32_t hb = t->mem->prog_break;

        if (stack_top <= hs || stack_bottom >= hb) {
            (void)proc_mem_register_stack_region(t->mem, stack_bottom, stack_top);
        }
    } else {
        (void)proc_mem_register_stack_region(t->mem, stack_bottom, stack_top);
    }

    proc_init_user_context(t, entry, user_esp);

    sched_add(t);
    return t;
}

static void proc_add_mmap_region(task_t* t, vfs_node_t* node, uint32_t vaddr, uint32_t size, uint32_t file_size, uint32_t offset) {
    mmap_area_t* area = static_cast<mmap_area_t*>(kmalloc(sizeof(mmap_area_t)));
    if (!area) return;

    uint32_t aligned_vaddr = vaddr & ~proc::detail::page_mask;
    
    uint32_t diff = vaddr - aligned_vaddr;
    
    uint32_t aligned_offset = offset - diff;
    
    uint32_t aligned_size = (size + diff + proc::detail::page_mask) & ~proc::detail::page_mask;

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
    
    if (t->mem) {
        vfs_node_retain(node);
        area->next = t->mem->mmap_list;
        t->mem->mmap_list = area;
    } else {
        kfree(area);
    }
}

static int proc_mem_has_mmap_overlap(proc_mem_t* mem, uint32_t start, uint32_t end_excl) {
    if (!mem || end_excl <= start) return 0;

    mmap_area_t* m = mem->mmap_list;
    while (m) {
        uint32_t a_start = m->vaddr_start;
        uint32_t a_end_excl = m->vaddr_end;

        if (a_start < end_excl && start < a_end_excl) {
            return 1;
        }

        m = m->next;
    }

    return 0;
}

static int proc_mem_register_stack_region(proc_mem_t* mem, uint32_t stack_bottom, uint32_t stack_top) {
    if (!mem || !mem->page_dir) return 0;
    if (stack_top <= stack_bottom) return 0;

    uint32_t start = stack_bottom & ~proc::detail::page_mask;
    uint32_t end_excl = (stack_top + proc::detail::page_mask) & ~proc::detail::page_mask;

    if (end_excl <= start) return 0;
    if (start < proc::detail::user_stack_addr_min || end_excl > proc::detail::user_stack_addr_max) return 0;

    if (proc_mem_has_mmap_overlap(mem, start, end_excl)) {
        return 1;
    }

    mmap_area_t* area = static_cast<mmap_area_t*>(kmalloc(sizeof(*area)));
    if (!area) return 0;

    memset(area, 0, sizeof(*area));
    area->vaddr_start = start;
    area->vaddr_end = end_excl;
    area->file_offset = 0;
    area->length = stack_top - stack_bottom;
    area->file_size = 0;
    area->map_flags = MAP_PRIVATE | MAP_STACK;
    area->file = 0;

    area->next = mem->mmap_list;
    mem->mmap_list = area;

    return 1;
}

task_t* proc_spawn_elf(const char* filename, int argc, char** argv) {
    kernel::unique_ptr<vfs_node_t, proc::detail::VfsNodeReleaseDeleter> exec_node(vfs_create_node_from_path(filename));
    if (!exec_node) return 0;

    Elf32_Ehdr header;
    kernel::unique_ptr<Elf32_Phdr, proc::detail::KfreeDeleter<Elf32_Phdr>> phdrs;
    uint32_t max_vaddr = 0;

    if (exec_node->ops->read(exec_node.get(), 0, sizeof(Elf32_Ehdr), &header) < (int)sizeof(Elf32_Ehdr)) {
        return 0;
    }
    
    if (header.e_ident[0] != 0x7F || header.e_ident[1] != 'E' || header.e_ident[2] != 'L' || header.e_ident[3] != 'F') {
        return 0;
    }
    if (header.e_ident[EI_CLASS] != ELFCLASS32 || header.e_ident[EI_DATA] != ELFDATA2LSB || header.e_ident[EI_VERSION] != EV_CURRENT) {
        return 0;
    }
    if (header.e_type != ET_EXEC || header.e_machine != EM_386 || header.e_version != EV_CURRENT) {
        return 0;
    }
    if (header.e_ehsize != sizeof(Elf32_Ehdr) || header.e_phentsize != sizeof(Elf32_Phdr)) {
        return 0;
    }
    if (header.e_phnum == 0 || header.e_phnum > proc::detail::max_elf_phdrs) {
        return 0;
    }
    {
        uint64_t ph_end = (uint64_t)header.e_phoff + (uint64_t)header.e_phnum * (uint64_t)sizeof(Elf32_Phdr);
        if (header.e_phoff == 0 || ph_end > (uint64_t)exec_node->size) {
            return 0;
        }
    }

    size_t phdr_bytes = (size_t)header.e_phnum * sizeof(Elf32_Phdr);
    phdrs.reset(static_cast<Elf32_Phdr*>(kmalloc(phdr_bytes)));
    if (!phdrs) {
        return 0;
    }
    if (exec_node->ops->read(exec_node.get(), header.e_phoff, (uint32_t)phdr_bytes, phdrs.get()) < (int)phdr_bytes) {
        return 0;
    }

    int have_load = 0;
    int entry_ok = 0;
    for (int i = 0; i < header.e_phnum; i++) {
        if (phdrs.get()[i].p_type != PT_LOAD) continue;

        uint32_t start_v = phdrs.get()[i].p_vaddr;
        uint32_t mem_sz  = phdrs.get()[i].p_memsz;
        uint32_t file_off= phdrs.get()[i].p_offset;
        uint32_t file_sz = phdrs.get()[i].p_filesz;

        if (mem_sz < file_sz) { return 0; }

        uint32_t end_v = start_v + mem_sz;
        if (end_v < start_v) { return 0; }
        if (start_v < proc::detail::user_elf_min_vaddr || end_v > proc::detail::user_elf_max_vaddr) { return 0; }

        uint32_t diff = start_v & proc::detail::page_mask;
        if (file_off < diff) { return 0; }

        uint64_t file_end = (uint64_t)file_off + (uint64_t)file_sz;
        if (file_end > (uint64_t)exec_node->size) { return 0; }

        have_load = 1;
        if (end_v > max_vaddr) max_vaddr = end_v;
        if (header.e_entry >= start_v && header.e_entry < end_v) entry_ok = 1;
    }
    if (!have_load || !entry_ok || max_vaddr == 0) {
        return 0;
    }

    char** k_argv = static_cast<char**>(kmalloc((argc + 1) * sizeof(char*)));
    if (!k_argv) {
        return 0;
    }

    struct KargvDeleter {
        int argc = 0;

        void operator()(char** argv) const noexcept {
            if (!argv) {
                return;
            }

            for (int i = 0; i < argc; i++) {
                if (argv[i]) {
                    kfree(argv[i]);
                    argv[i] = nullptr;
                }
            }

            kfree(argv);
        }
    };

    kernel::unique_ptr<char*, KargvDeleter> kargv_guard(k_argv, KargvDeleter{argc});
    
    for (int i = 0; i < argc; i++) {
        int len = strlen(argv[i]) + 1;
        k_argv[i] = static_cast<char*>(kmalloc(len));
        memcpy(k_argv[i], argv[i], len);
    }
    k_argv[argc] = 0;

    struct TaskFreeDeleter {
        void operator()(task_t* t) const noexcept {
            if (t) {
                proc_free_resources(t);
            }
        }
    };

    kernel::unique_ptr<task_t, TaskFreeDeleter> t_guard(alloc_task());
    if (!t_guard) {
        return 0;
    }

    task_t* t = t_guard.get();

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
        fd_table_t* cloned = proc_fd_table_clone(parent->fd_table);
        if (!cloned) {
            return 0;
        }

        proc_fd_table_release(t->fd_table);
        t->fd_table = cloned;
    } else {
        file_desc_t* f0 = 0;
        file_desc_t* f1 = 0;
        file_desc_t* f2 = 0;
        if (proc_fd_add_at(t, 0, &f0) >= 0 && f0) {
            f0->node = devfs_clone("kbd");
            f0->offset = 0;
            f0->flags = 0;
        }
        if (proc_fd_add_at(t, 1, &f1) >= 0 && f1) {
            f1->node = devfs_clone("console");
            f1->offset = 0;
            f1->flags = 0;
        }
        if (proc_fd_add_at(t, 2, &f2) >= 0 && f2 && f1 && f1->node) {
            vfs_node_t* node = f1->node;
            vfs_node_retain(node);
            f2->node = node;
            f2->offset = f1->offset;
            f2->flags = f1->flags;
        }
    }
    
    t->priority = PRIO_USER;
    strlcpy(t->name, filename, 32);

    t->mem = proc_mem_create(t->pid);
    if (!t->mem) {
        return 0;
    }

    t->kstack_size = KSTACK_SIZE;
    kernel::unique_ptr<uint8_t, proc::detail::KfreeDeleter<uint8_t>> kstack_guard(
        static_cast<uint8_t*>(kmalloc_a(t->kstack_size))
    );
    if (!kstack_guard) {
        return 0;
    }
    memset(kstack_guard.get(), 0, t->kstack_size);
    t->kstack = kstack_guard.release();

    for (int i = 0; i < header.e_phnum; i++) {
        if (phdrs.get()[i].p_type == 1) {
            uint32_t start_v = phdrs.get()[i].p_vaddr;
            uint32_t mem_sz  = phdrs.get()[i].p_memsz;
            uint32_t file_off= phdrs.get()[i].p_offset;
            uint32_t file_sz = phdrs.get()[i].p_filesz;
            
            proc_add_mmap_region(t, exec_node.get(), start_v, mem_sz, file_sz, file_off);
        }
    }

    uint32_t start_pde_idx = proc::detail::user_elf_min_vaddr >> 22;
    uint32_t end_pde_idx   = (max_vaddr - 1) >> 22;

    for (uint32_t i = start_pde_idx; i <= end_pde_idx; i++) {
        t->mem->page_dir[i] = 0;
    }

    t->mem->prog_break = (max_vaddr + proc::detail::page_mask) & ~proc::detail::page_mask;
    t->mem->heap_start = t->mem->prog_break;

    if (t->mem->mmap_top < t->mem->prog_break) t->mem->mmap_top = t->mem->prog_break + proc::detail::heap_mmap_bump;

    uint32_t stack_size = proc::detail::user_stack_size_bytes;
    uint32_t ustack_top_limit = proc::detail::user_stack_top_limit;
    uint32_t ustack_bottom = ustack_top_limit - stack_size;

    t->stack_bottom = ustack_bottom;
    t->stack_top = ustack_top_limit;

    (void)proc_mem_register_stack_region(t->mem, t->stack_bottom, t->stack_top);

    for (int i = 1; i <= 4; i++) {
        uint32_t addr = ustack_top_limit - (uint32_t)i * proc::detail::page_size;
        void* p = pmm_alloc_block();
        if (p) {
            paging_map(t->mem->page_dir, addr, (uint32_t)p, 7);
            t->mem->mem_pages++;
        }
    }

    uint32_t final_user_esp = 0;

    {
        proc::detail::ScopedIrqDisable irq_guard;
        proc::detail::ScopedPagingSwitch paging_guard(t->mem->page_dir);

        uint32_t ustack_top = ustack_top_limit;

        uint32_t arg_ptrs[proc::detail::max_user_stack_argc];
        int actual_argc = (argc > proc::detail::max_user_stack_argc) ? proc::detail::max_user_stack_argc : argc;

        for (int i = actual_argc - 1; i >= 0; i--) {
            size_t len = strlen(k_argv[i]) + 1;
            ustack_top -= len;
            memcpy((void*)ustack_top, k_argv[i], len);
            arg_ptrs[i] = ustack_top;
        }

        ustack_top &= ~proc::detail::stack_align_mask;
        uint32_t* us = (uint32_t*)ustack_top;

        *--us = 0;

        for (int i = actual_argc - 1; i >= 0; i--) *--us = arg_ptrs[i];

        uint32_t argv_ptr = (uint32_t)us;

        *--us = argv_ptr;
        *--us = (uint32_t)actual_argc;
        *--us = 0;

        final_user_esp = (uint32_t)us;
    }

    proc_init_user_context(t, header.e_entry, final_user_esp);

    task_t* out = t_guard.release();
    sched_add(out);
    return out;
}

void proc_wait(uint32_t pid) {
    task_t* target = proc_find_by_pid(pid);
    if (!target) return;

    task_t* waiter = proc_current();
    if (waiter) waiter->wait_for_pid = pid;

    __sync_fetch_and_add(&target->exit_waiters, 1);
    
    sem_wait(&target->exit_sem);

    if (waiter) waiter->wait_for_pid = 0;
    __sync_fetch_and_sub(&target->exit_waiters, 1);
}

int proc_waitpid(uint32_t pid, int* out_status) {
    task_t* target = proc_find_by_pid(pid);
    if (!target) return -1;

    task_t* waiter = proc_current();
    if (waiter) waiter->wait_for_pid = pid;

    __sync_fetch_and_add(&target->exit_waiters, 1);

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
    while (1) {
        int freed = 0;
        do {
            freed = 0;
            task_t* victim = 0;

            {
                kernel::SpinLockSafeGuard guard(proc::detail::zombie_lock);

                dlist_head_t* it = proc::detail::zombie_list.next;
                while (it && it != &proc::detail::zombie_list) {
                    task_t* curr = container_of(it, task_t, zombie_node);
                    it = it->next;

                    int still_running = 0;
                    for (int i = 0; i < MAX_CPUS; i++) {
                        if (cpus[i].current_task == curr) {
                            still_running = 1;
                            break;
                        }
                    }

                    if (still_running || curr->exit_waiters > 0) {
                        continue;
                    }

                    dlist_del(&curr->zombie_node);
                    victim = curr;
                    break;
                }
            }

            if (victim) {
                proc_free_resources(victim);
                freed = 1;
            }
        } while (freed);

        proc_usleep(50000);
    }
}

task_t* proc_create_idle(int cpu_index) {
    task_t* t = alloc_task();
    if (!t) return 0;

    const uint32_t old_pid = t->pid;

    strlcpy(t->name, "idle", 32);
    t->state = TASK_RUNNING;
    t->pid = 0;             
    t->assigned_cpu = cpu_index;
    t->mem = 0;
    t->priority = PRIO_IDLE;

    if (!proc_alloc_kstack(t)) {
        proc_free_resources(t);
        return 0;
    }

    uint32_t* sp = proc_kstack_top(t);
    
    *--sp = 0;
    *--sp = 0; // Fake Return Address
    *--sp = (uint32_t)idle_task_func;
    *--sp = 0; // EBP
    *--sp = 0; // EBX
    *--sp = 0; // ESI
    *--sp = 0; // EDI
    
    t->esp = sp;

    kernel::SpinLockSafeGuard guard(proc::detail::proc_lock);
    dlist_del(&t->all_tasks_node);
    if (proc::detail::total_tasks > 0) {
        proc::detail::total_tasks--;
    }
    proc::detail::pid_map_remove(old_pid);

    return t;
}

static void insert_sleeper(task_t* t) {
    if (!t) {
        return;
    }

    (void)proc::detail::sleeping_tree.insert_unique(*t);
}

void proc_sleep_add(task_t* t, uint32_t wake_tick) {
    {
        kernel::SpinLockSafeGuard guard(proc::detail::sleep_lock);

        if (t->wake_tick != 0) {
            proc::detail::sleeping_tree.erase(*t);
        }

        t->wake_tick = wake_tick;
        t->state = TASK_WAITING;

        insert_sleeper(t);
    }

    sched_yield();
}

void proc_usleep(uint32_t us) {
    task_t* curr = proc_current();
    if (!curr) return;

    uint32_t ticks = (uint32_t)(((uint64_t)us * KERNEL_TIMER_HZ) / 1000000ull);
    if (ticks == 0) ticks = 1;

    uint32_t target = timer_ticks + ticks;
    proc_sleep_add(curr, target);
}

void proc_check_sleepers(uint32_t current_tick) {
    kernel::TrySpinLockGuard guard(proc::detail::sleep_lock);
    if (guard) {
        while (!proc::detail::sleeping_tree.empty()) {
            task_t& t = *proc::detail::sleeping_tree.begin();
            if (t.wake_tick > current_tick) {
                break;
            }

            proc::detail::sleeping_tree.erase(t);
            t.state = TASK_RUNNABLE;
            t.wake_tick = 0;
            sched_add(&t);
        }
    }
}

void proc_sleep_remove(task_t* t) {
    kernel::SpinLockSafeGuard guard(proc::detail::sleep_lock);
    
    if (t->wake_tick != 0) {
        proc::detail::sleeping_tree.erase(*t);
        t->wake_tick = 0;
    }
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
