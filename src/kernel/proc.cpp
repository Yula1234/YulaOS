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
#include <mm/heap.h>
#include <mm/vma.h>

#include <hal/io.h>
#include <hal/simd.h>
#include <hal/apic.h>
#include <drivers/fbdev.h>
#include <kernel/input_focus.h>

#include <lib/hash_map.h>
#include <lib/cpp/dlist.h>
#include <lib/cpp/atomic.h>
#include <lib/cpp/lock_guard.h>
#include <lib/cpp/unique_ptr.h>

#include "sched.h"
#include "proc.h"
#include <kernel/waitq/poll_waitq.h>
#include <kernel/sched.h>
#include <kernel/syscall.h>
#include <kernel/futex/futex.h>
#include "elf.h"
#include <kernel/smp/cpu.h>

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

static constexpr uint32_t user_elf_min_vaddr = 0x40000000u;
static constexpr uint32_t user_elf_max_vaddr = 0xB0000000u;

static constexpr uint32_t user_stack_addr_min = 0x40000000u;
static constexpr uint32_t user_stack_addr_max = 0xC0000000u;

static constexpr uint32_t default_mmap_top = user_elf_min_vaddr;

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
static kernel::atomic<uint32_t> g_next_pid{1u};

static percpu_rwspinlock_t task_list_lock;

struct ProcGroup {
    uint32_t pgid = 0;
    rwspinlock_t lock{};
    dlist_head_t members{};
    kernel::atomic<uint32_t> refs{0u};
};

static HashMap<uint32_t, ProcGroup*, PID_MAP_BUCKETS> pgroups;

static inline void pgroup_ref(ProcGroup* g) {
    if (!g) {
        return;
    }

    g->refs.fetch_add(1u, kernel::memory_order::relaxed);
}

static inline void pgroup_put(ProcGroup* g) {
    if (!g) {
        return;
    }

    const uint32_t prev = g->refs.fetch_sub(1u, kernel::memory_order::acq_rel);
    if (prev == 1u) {
        kfree(g);
    }
}

static ProcGroup* pgroup_acquire(uint32_t pgid) {
    if (pgid == 0) {
        return nullptr;
    }

    ProcGroup* g = nullptr;

    {
        auto locked = pgroups.find_ptr(pgid);
        if (!locked) {
            return nullptr;
        }

        auto* v = locked.value_ptr();
        if (!v || !*v) {
            return nullptr;
        }

        g = *v;
        pgroup_ref(g);
    }

    return g;
}

static ProcGroup* pgroup_get_or_create(uint32_t pgid) {
    if (pgid == 0) {
        return nullptr;
    }

    for (;;) {
        if (ProcGroup* existing = pgroup_acquire(pgid)) {
            return existing;
        }

        auto* group = static_cast<ProcGroup*>(kmalloc(sizeof(ProcGroup)));
        if (!group) {
            return nullptr;
        }

        memset(group, 0, sizeof(*group));

        group->pgid = pgid;
        
        rwspinlock_init(&group->lock);
        dlist_init(&group->members);
        
        group->refs.store(2u, kernel::memory_order::relaxed);

        const auto res = pgroups.insert_unique_ex(pgid, group);
        if (res == HashMap<uint32_t, ProcGroup*, PID_MAP_BUCKETS>::InsertUniqueResult::Inserted) {
            return group;
        }

        if (res == HashMap<uint32_t, ProcGroup*, PID_MAP_BUCKETS>::InsertUniqueResult::AlreadyPresent) {
            kfree(group);
            continue;
        }

        kfree(group);
        return nullptr;
    }
}

static void pgroup_remove_if_empty_locked(ProcGroup* g) {
    if (!g) {
        return;
    }

    if (!dlist_empty(&g->members)) {
        return;
    }

    const uint32_t refs = g->refs.load(kernel::memory_order::relaxed);
    if (refs != 2u) {
        return;
    }

    ProcGroup* removed = nullptr;
    if (!pgroups.remove_and_get(g->pgid, removed)) {
        return;
    }

    if (removed != g) {
        if (removed) {
            pgroups.insert(g->pgid, removed);
        }
        return;
    }

    pgroup_put(g);
}

static void task_pgroup_detach_locked(task_t* t) {
    if (!t) {
        return;
    }

    if (!t->pgrp_node.next || !t->pgrp_node.prev) {
        return;
    }

    if (t->pgid != 0) {
        ProcGroup* g = pgroup_acquire(t->pgid);
        if (g) {
            {
                kernel::RwSpinLockNativeWriteSafeGuard group_guard(g->lock);

                dlist_del(&t->pgrp_node);
                t->pgrp_node.next = nullptr;
                t->pgrp_node.prev = nullptr;

                pgroup_remove_if_empty_locked(g);
            }

            pgroup_put(g);
            return;
        }
    }

    dlist_del(&t->pgrp_node);
    t->pgrp_node.next = nullptr;
    t->pgrp_node.prev = nullptr;
}

static void task_parent_detach_locked(task_t* t) {
    if (!t) {
        return;
    }

    if (!t->sibling_node.next || !t->sibling_node.prev) {
        return;
    }

    dlist_del(&t->sibling_node);
    t->sibling_node.next = nullptr;
    t->sibling_node.prev = nullptr;

    proc_task_put(t);
}

static void task_set_parent_locked(task_t* child, task_t* parent) {
    if (!child) {
        return;
    }

    if (parent && parent != child) {
        if (!proc_task_retain(child)) {
            child->parent_pid = 0;
            return;
        }

        task_parent_detach_locked(child);

        dlist_add_tail(&child->sibling_node, &parent->children_list);
        child->parent_pid = parent->pid;
        return;
    }

    child->parent_pid = 0;
    task_parent_detach_locked(child);
}

static int task_set_pgid_locked(task_t* t, uint32_t pgid) {
    if (!t || pgid == 0) {
        return -1;
    }

    if (t->pgid == pgid) {
        return 0;
    }

    task_pgroup_detach_locked(t);

    ProcGroup* g = pgroup_get_or_create(pgid);
    if (!g) {
        return -1;
    }

    {
        kernel::RwSpinLockNativeWriteSafeGuard group_guard(g->lock);

        dlist_add_tail(&t->pgrp_node, &g->members);
        t->pgid = pgid;
    }

    pgroup_put(g);
    return 0;
}

static void task_inherit_process_context_locked(task_t* child, const task_t* parent) {
    if (!child || !parent) {
        return;
    }

    child->sid = parent->sid;
    (void)task_set_pgid_locked(child, parent->pgid);

    if (child->controlling_tty) {
        vfs_node_release(child->controlling_tty);
        child->controlling_tty = nullptr;
    }

    if (parent->controlling_tty) {
        vfs_node_retain(parent->controlling_tty);
        child->controlling_tty = parent->controlling_tty;
    }
}

static inline bool sleep_less(const task_t* a, const task_t* b) noexcept {
    if (a->wake_tick != b->wake_tick) {
        return a->wake_tick < b->wake_tick;
    }

    return reinterpret_cast<uintptr_t>(a) < reinterpret_cast<uintptr_t>(b);
}

static inline void cpu_sleep_update_next_wake_tick(cpu_t* cpu) noexcept {
    if (!cpu || !cpu->sleep_leftmost) {
        if (cpu) {
            cpu->sleep_next_wake_tick = 0xFFFFFFFFu;
        }

        return;
    }

    cpu->sleep_next_wake_tick = cpu->sleep_leftmost->wake_tick;
}

static void cpu_sleep_insert_locked(cpu_t* cpu, task_t* t) noexcept {
    struct rb_node** link = &cpu->sleep_root.rb_node;
    struct rb_node* parent = nullptr;
    task_t* entry = nullptr;
    bool leftmost = true;

    while (*link) {
        parent = *link;
        entry = rb_entry(parent, task_t, sleep_rb);

        if (sleep_less(t, entry)) {
            link = &parent->rb_left;
        } else {
            link = &parent->rb_right;
            leftmost = false;
        }
    }

    rb_link_node(&t->sleep_rb, parent, link);
    rb_insert_color(&t->sleep_rb, &cpu->sleep_root);

    if (leftmost) {
        cpu->sleep_leftmost = t;
    }

    cpu_sleep_update_next_wake_tick(cpu);
}

static void cpu_sleep_remove_locked(cpu_t* cpu, task_t* t) noexcept {
    if (!cpu || !t || t->wake_tick == 0) {
        return;
    }

    if (cpu->sleep_leftmost == t) {
        struct rb_node* next = rb_next(&t->sleep_rb);
        cpu->sleep_leftmost = next ? rb_entry(next, task_t, sleep_rb) : nullptr;
    }

    rb_erase(&t->sleep_rb, &cpu->sleep_root);

    cpu_sleep_update_next_wake_tick(cpu);
}

int proc_setsid(task_t* t) {
    if (!t) {
        return -1;
    }

    kernel::ScopedIrqDisable irq_guard;

    {
        kernel::SpinLockNativeGuard guard(t->state_lock);

        if (t->pid == t->pgid) {
            return -1;
        }

        t->sid = t->pid;
        (void)task_set_pgid_locked(t, t->pid);

        if (t->controlling_tty) {
            vfs_node_release(t->controlling_tty);
            t->controlling_tty = nullptr;
        }

    }

    return (int)t->sid;
}

int proc_setpgid(task_t* t, uint32_t pgid) {
    if (!t || pgid == 0) {
        return -1;
    }

    int rc;

    kernel::ScopedIrqDisable irq_guard;

    {
        kernel::SpinLockNativeGuard guard(t->state_lock);

        rc = task_set_pgid_locked(t, pgid);
    }

    return rc;
}

uint32_t proc_getpgrp(task_t* t) {
    if (!t) {
        return 0;
    }

    return t->pgid;
}

int proc_signal_pgrp(uint32_t pgid, uint32_t sig) {
    if (pgid == 0 || sig >= 32u) {
        return -1;
    }

    ProcGroup* g = pgroup_acquire(pgid);
    if (!g) {
        return -1;
    }

    kernel::RwSpinLockNativeReadSafeGuard group_guard(g->lock);

    int signaled = 0;
    for (dlist_head_t* it = g->members.next; it && it != &g->members; it = it->next) {
        task_t* m = container_of(it, task_t, pgrp_node);
        if (!m || m->state == TASK_UNUSED || m->state == TASK_ZOMBIE) {
            continue;
        }

        __sync_fetch_and_or(&m->pending_signals, 1u << sig);
        proc_wake(m);
        signaled++;
    }

    pgroup_put(g);
    return signaled;
}

int proc_pgrp_in_session(uint32_t pgid, uint32_t sid) {
    if (pgid == 0 || sid == 0) {
        return 0;
    }

    ProcGroup* g = pgroup_acquire(pgid);
    if (!g) {
        return 0;
    }

    kernel::RwSpinLockNativeReadSafeGuard group_guard(g->lock);

    for (dlist_head_t* it = g->members.next; it && it != &g->members; it = it->next) {
        task_t* m = container_of(it, task_t, pgrp_node);
        if (!m) {
            continue;
        }

        if (m->state == TASK_UNUSED || m->state == TASK_ZOMBIE) {
            continue;
        }

        if (m->sid == sid) {
            pgroup_put(g);
            return 1;
        }
    }

    pgroup_put(g);
    return 0;
}

static spinlock_t g_zombie_lock;
static task_t* g_zombie_head = nullptr;

static HashMap<uint32_t, task_t*, PID_MAP_BUCKETS> pid_map;

static uint8_t* initial_fpu_state = 0;
static uint32_t initial_fpu_state_size = 0;

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

    if (!::proc_task_retain(t)) {
        return;
    }

    const auto res = pid_map.insert_unique_ex(pid, t);
    if (res != decltype(pid_map)::InsertUniqueResult::Inserted) {
        ::proc_task_put(t);
    }
}

static void pid_map_remove(uint32_t pid) {
    task_t* removed = nullptr;
    if (!pid_map.remove_and_get(pid, removed)) {
        return;
    }

    if (removed) {
        ::proc_task_put(removed);
    }
}

}
}

extern "C" int proc_setsid(task_t* t) {
    return proc::detail::proc_setsid(t);
}

extern "C" int proc_setpgid(task_t* t, uint32_t pgid) {
    return proc::detail::proc_setpgid(t, pgid);
}

extern "C" uint32_t proc_getpgrp(task_t* t) {
    return proc::detail::proc_getpgrp(t);
}

extern "C" int proc_signal_pgrp(uint32_t pgid, uint32_t sig) {
    return proc::detail::proc_signal_pgrp(pgid, sig);
}

extern "C" int proc_pgrp_in_session(uint32_t pgid, uint32_t sid) {
    return proc::detail::proc_pgrp_in_session(pgid, sid);
}

extern "C" void irq_return(void);
extern "C" void idle_task_func(void*);
extern volatile uint32_t timer_ticks;

uint32_t proc_list_snapshot(yos_proc_info_t* out, uint32_t cap) {
    if (!out || cap == 0) return 0;

    uint32_t count = 0;

    kernel::PerCpuRwSpinLockNativeReadSafeGuard guard(proc::detail::task_list_lock);

    for (task_t& t : proc::detail::all_tasks) {
        if (count >= cap) {
            break;
        }

        if (t.state == TASK_UNUSED) {
            continue;
        }

        uint32_t parent_pid;
        {
            kernel::SpinLockNativeSafeGuard guard(t.state_lock);
            parent_pid = t.parent_pid;
        }

        yos_proc_info_t* e = &out[count++];
        e->pid = t.pid;
        e->parent_pid = parent_pid;
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

    proc::detail::g_next_pid.store(1u, kernel::memory_order::relaxed);

    spinlock_init(&proc::detail::g_zombie_lock);
    proc::detail::g_zombie_head = nullptr;

    percpu_rwspinlock_init(&proc::detail::task_list_lock);

    proc::detail::all_tasks.clear_links_unsafe();

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
    rwspinlock_init(&ft->lock);
    
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

    for (;;) {
        uint32_t expected = __atomic_load_n(&ft->refs, __ATOMIC_RELAXED);
        if (expected == 0u) {
            panic("PROC: fd_table_retain after free");
        }

        if (__atomic_compare_exchange_n(
                &ft->refs,
                &expected,
                expected + 1u,
                false,
                __ATOMIC_ACQ_REL,
                __ATOMIC_RELAXED
            )) {
            return;
        }
    }
}

void file_desc_retain(file_desc_t* d) {
    if (!d) return;

    for (;;) {
        uint32_t expected = __atomic_load_n(&d->refs, __ATOMIC_RELAXED);
        if (expected == 0u) {
            panic("PROC: file_desc_retain after free");
        }

        if (__atomic_compare_exchange_n(
                &d->refs,
                &expected,
                expected + 1u,
                false,
                __ATOMIC_ACQ_REL,
                __ATOMIC_RELAXED
            )) {
            return;
        }
    }
}

void file_desc_release(file_desc_t* d) {
    if (!d) return;

    for (;;) {
        uint32_t expected = __atomic_load_n(&d->refs, __ATOMIC_RELAXED);
        if (expected == 0u) {
            panic("PROC: file_desc_release underflow");
        }

        const uint32_t desired = expected - 1u;

        if (__atomic_compare_exchange_n(
                &d->refs,
                &expected,
                desired,
                false,
                __ATOMIC_ACQ_REL,
                __ATOMIC_RELAXED
            )) {
            if (desired != 0u) {
                return;
            }

            break;
        }
    }

    if (d->node) {
        vfs_node_release(d->node);
        d->node = 0;
    }

    kfree(d);
}

void proc_fd_table_release(fd_table_t* ft) {
    if (!ft) return;

    for (;;) {
        uint32_t expected = __atomic_load_n(&ft->refs, __ATOMIC_RELAXED);
        if (expected == 0u) {
            panic("PROC: fd_table_release underflow");
        }

        const uint32_t desired = expected - 1u;

        if (__atomic_compare_exchange_n(
                &ft->refs,
                &expected,
                desired,
                false,
                __ATOMIC_ACQ_REL,
                __ATOMIC_RELAXED
            )) {
            if (desired != 0u) {
                return;
            }

            break;
        }
    }

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

    kernel::RwSpinLockNativeReadGuard guard(ft->lock);
    
    file_desc_t* out = 0;
    if ((uint32_t)fd < ft->max_fds && ft->fds) {
        out = ft->fds[fd];
    }

    if (out) {
        file_desc_retain(out);
    }

    return out;
}

static int fd_table_ensure_cap(fd_table_t* ft, uint32_t required_fd) {
    if (required_fd < ft->max_fds) return 0;
    
    uint32_t new_cap = ft->max_fds ? ft->max_fds : 32;
    while (new_cap <= required_fd) {
        if (new_cap >= proc::detail::fd_table_cap_limit) {
            return -1;
        }

        new_cap *= 2;
    }
    
    file_desc_t** new_fds = static_cast<file_desc_t**>(kmalloc(sizeof(file_desc_t*) * new_cap));
    if (!new_fds) {
        return -1;
    }
    
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

    kernel::RwSpinLockNativeWriteGuard guard(ft->lock);
    
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

    if (fd >= ft->fd_next) {
        ft->fd_next = fd + 1;
    }
    
    if (out_desc) *out_desc = d;
    return fd;
}

int proc_fd_install_at(task_t* t, int fd, file_desc_t* desc) {
    if (!t || fd < 0 || !desc) return -1;

    fd_table_t* ft = t->fd_table;
    if (!ft) return -1;

    kernel::RwSpinLockNativeWriteGuard guard(ft->lock);
    
    if (fd_table_ensure_cap(ft, (uint32_t)fd) != 0) {
        return -1;
    }

    if (ft->fds[fd]) {
        return -1;
    }

    ft->fds[fd] = desc;
    file_desc_retain(desc);

    if (fd >= ft->fd_next) {
        ft->fd_next = fd + 1;
    }
    
    return fd;
}

int proc_fd_alloc(task_t* t, file_desc_t** out_desc) {
    if (out_desc) *out_desc = 0;
    if (!t) return -1;

    fd_table_t* ft = t->fd_table;
    if (!ft) return -1;

    int found = -1;

    {
        kernel::RwSpinLockNativeWriteGuard guard(ft->lock);

        int expected = ft->fd_next;
        if (expected < 0) {
            expected = 0;
        }

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

    kernel::RwSpinLockNativeWriteGuard guard(ft->lock);
    
    if ((uint32_t)fd >= ft->max_fds || !ft->fds || !ft->fds[fd]) {
        return -1;
    }

    file_desc_t* d = ft->fds[fd];
    ft->fds[fd] = 0;

    if (fd < ft->fd_next) {
        ft->fd_next = fd;
    }

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
    rwspinlock_init(&ft.get()->lock);

    kernel::RwSpinLockNativeReadGuard src_guard(src->lock);
    
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
    task_t* out = nullptr;

    if (!proc::detail::pid_map.with_value_locked(
        pid,
        [&out](task_t* t) -> bool {
            if (!t || t->state == TASK_UNUSED) {
                return false;
            }

            if (!proc_task_retain(t)) {
                return false;
            }
            out = t;
            return true;
        }
    )) {
        return nullptr;
    }

    return out;
}

int proc_task_retain(task_t* t) {
    if (!t) {
        return 0;
    }

    for (;;) {
        uint32_t expected = __atomic_load_n(&t->refs, __ATOMIC_RELAXED);
        if (expected == 0u) {
            return 0;
        }

        if (__atomic_compare_exchange_n(
            &t->refs,
            &expected,
            expected + 1u,
            0,
            __ATOMIC_ACQ_REL,
            __ATOMIC_RELAXED
        )) {
            return 1;
        }
    }
}

void proc_task_put(task_t* t) {
    if (!t) {
        return;
    }

    const uint32_t old = __sync_fetch_and_sub(&t->refs, 1u);
    if (old == 0u) {
        __sync_fetch_and_add(&t->refs, 1u);
        return;
    }
    if (old != 1u) {
        return;
    }

    kfree(t);
}

static proc_mem_t* proc_mem_create(uint32_t leader_pid) {
    kernel::unique_ptr<proc_mem_t, proc::detail::KfreeDeleter<proc_mem_t>> mem_guard(
        static_cast<proc_mem_t*>(kmalloc_a(sizeof(proc_mem_t)))
    );
    if (!mem_guard) return 0;

    proc_mem_t* mem = mem_guard.get();
    memset(mem, 0, sizeof(*mem));

    spinlock_init(&mem->pt_lock);

    mem->leader_pid = leader_pid;
    mem->refcount = 1;
    mem->mmap_top = proc::detail::default_mmap_top;

    vma_init(mem);

    mem->page_dir = paging_clone_directory();
    if (!mem->page_dir) {
        return 0;
    }

    (void)paging_register_dir_lock(mem->page_dir, &mem->pt_lock);

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

    vma_destroy(mem);

    if (mem->page_dir && mem->page_dir != kernel_page_directory) {
        paging_unregister_dir_lock(mem->page_dir);

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

                    if ((pte & 1u) == 0u) {
                        continue;
                    }

                    const uint32_t phys = pte & ~0xFFFu;
                    const uint32_t flags = pte & 0xFFFu;

                    pt[j] = 0u;

                    if ((flags & 0x200u) != 0u) {
                        continue;
                    }

                    if ((flags & 4u) != 0u && phys != 0u) {
                        pmm_free_block((void*)phys);
                    }
                }

                pmm_free_block(pt);
            }
        }

        pmm_free_block(mem->page_dir);
        mem->page_dir = 0;
    }

    mem->mem_pages = 0;

    kfree(mem);
}

static task_t* alloc_task(void) {
    kernel::unique_ptr<task_t, proc::detail::KfreeDeleter<task_t>> t_guard(
        static_cast<task_t*>(kmalloc_a(sizeof(task_t)))
    );
    if (!t_guard) {
        return 0;
    }

    task_t* t = t_guard.get();
    memset(t, 0, sizeof(task_t));
    t->sleep_cpu = -1;

    proc_fd_table_init(t);

    spinlock_init(&t->poll_lock);
    dlist_init(&t->poll_waiters);

    t->pgrp_node.next = nullptr;
    t->pgrp_node.prev = nullptr;

    dlist_init(&t->children_list);

    t->sibling_node.next = nullptr;
    t->sibling_node.prev = nullptr;

    spinlock_init(&t->state_lock);

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

    t->refs = 1u;

    t->pid = proc::detail::g_next_pid.fetch_add(1u, kernel::memory_order::relaxed);
    if (t->pid == 0u) {
        t->pid = proc::detail::g_next_pid.fetch_add(1u, kernel::memory_order::relaxed);
    }

    t->state = TASK_RUNNABLE;
    t->cwd_inode = 1;
    t->term_mode = 0;
    t->assigned_cpu = -1;
    t->vruntime = 0;
    t->exec_start = 0;

    proc::detail::pid_map_insert(t->pid, t);

    t->sid = t->pid;

    {
        kernel::SpinLockNativeGuard guard(t->state_lock);

        (void)proc::detail::task_set_pgid_locked(t, t->pid);
    }

    {
        kernel::PerCpuRwSpinLockNativeWriteGuard guard(proc::detail::task_list_lock);

        proc_task_retain(t);
        proc::detail::all_tasks.push_back(*t);
        proc::detail::total_tasks++;
    }

    return t_guard.release();
}

void proc_free_resources(task_t* t) {
    if (!t) return;

    if (!proc_task_retain(t)) {
        return;
    }

    proc_sleep_remove(t);
    poll_task_cleanup(t);

    vfs_node_t* tty_node = nullptr;
    if (t->controlling_tty) {
        tty_node = t->controlling_tty;
        t->controlling_tty = nullptr;
    }

    if (t->fd_table) {
        proc_fd_table_release(t->fd_table);
        t->fd_table = 0;
    }

    {
        kernel::SpinLockNativeSafeGuard guard(t->state_lock);
        
        proc::detail::task_pgroup_detach_locked(t);
    }

    {
        kernel::ScopedIrqDisable irq_guard;

        uint32_t parent_pid;

        {
            kernel::SpinLockNativeGuard guard(t->state_lock);
            parent_pid = t->parent_pid;
        }

        if (parent_pid != 0) {
            task_t* parent = proc_find_by_pid(parent_pid);
            if (parent) {
                {
                    kernel::SpinLockNativeGuard parent_guard(parent->state_lock);
                    kernel::SpinLockNativeGuard t_guard(t->state_lock);

                    if (t->parent_pid == parent_pid) {
                        t->parent_pid = 0;

                        proc::detail::task_parent_detach_locked(t);
                    }
                }

                proc_task_put(parent);
            }
        } else {
            kernel::SpinLockNativeGuard guard(t->state_lock);

            t->parent_pid = 0;
            proc::detail::task_parent_detach_locked(t);
        }
    }

    if (tty_node) {
        vfs_node_release(tty_node);
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
    
    bool removed_from_all_tasks = false;
    {
        kernel::PerCpuRwSpinLockNativeWriteSafeGuard guard(proc::detail::task_list_lock);

        if (t->all_tasks_node.next && t->all_tasks_node.prev) {
            dlist_del(&t->all_tasks_node);
            removed_from_all_tasks = true;
        }
        if (removed_from_all_tasks && proc::detail::total_tasks > 0) {
            proc::detail::total_tasks--;
        }
    }

    if (removed_from_all_tasks) {
        proc_task_put(t);
    }

    proc_task_put(t);
}

void proc_kill(task_t* t) {
    if (!t) return;

    {
        kernel::SpinLockNativeSafeGuard guard(t->state_lock);

        if (t->state == TASK_ZOMBIE || t->state == TASK_UNUSED) {
            return;
        }

        t->state = TASK_ZOMBIE;
    }
    
    uint32_t waited_pid = t->wait_for_pid;
    if (waited_pid) {
        task_t* waited = proc_find_by_pid(waited_pid);
        if (waited) {
            uint32_t old = __sync_fetch_and_sub(&waited->exit_waiters, 1);
            if (old == 0) {
                __sync_fetch_and_add(&waited->exit_waiters, 1);
            }

            proc_task_put(waited);
        }
        t->wait_for_pid = 0;
    }

    task_t* init = proc_find_by_pid(1);
    if (init && init != t) {
        kernel::ScopedIrqDisable irq_guard;

        task_t* child = nullptr;
        task_t* next = nullptr;

        kernel::SpinLockNativeGuard init_guard(init->state_lock);
        kernel::SpinLockNativeGuard t_guard(t->state_lock);

        dlist_for_each_entry_safe(child, next, &t->children_list, sibling_node) {
            kernel::SpinLockNativeGuard child_guard(child->state_lock);

            if (child->state == TASK_UNUSED) {
                child->parent_pid = 0;

                proc::detail::task_parent_detach_locked(child);
                continue;
            }

            proc::detail::task_set_parent_locked(child, init);
        }
    }

    if (init) {
        proc_task_put(init);
    }

    if (t->blocked_kind == TASK_BLOCK_FUTEX) {
        futex_remove_task(t);
    } else {
        sem_remove_task(t);
    }

    poll_task_cleanup(t);

    proc_sleep_remove(t);

    sched_remove(t);

    if (!proc_task_retain(t)) {
        return;
    }

    {
        kernel::SpinLockNativeSafeGuard guard(proc::detail::g_zombie_lock);

        t->zombie_next = proc::detail::g_zombie_head;
        proc::detail::g_zombie_head = t;
    }

    sem_signal_all(&t->exit_sem);
}

static void kthread_trampoline(void) {
    task_t* t = proc_current();

    sched_on_task_entry();

    __asm__ volatile("sti");

    t->entry(t->arg);       

    proc_kill(t);

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

    proc_task_put(t);
    return t;
}

task_t* proc_get_list_head() {
    kernel::PerCpuRwSpinLockNativeReadSafeGuard guard(proc::detail::task_list_lock);

    if (proc::detail::all_tasks.empty()) {
        return 0;
    }

    return &proc::detail::all_tasks.front();
}
uint32_t proc_task_count(void) {
    kernel::PerCpuRwSpinLockNativeReadSafeGuard guard(proc::detail::task_list_lock);
    return proc::detail::total_tasks;
}

task_t* proc_task_at(uint32_t idx) {
    kernel::PerCpuRwSpinLockNativeReadSafeGuard guard(proc::detail::task_list_lock);

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
    if (!kstack_guard) {
        return 0;
    }

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
        kernel::ScopedIrqDisable irq_guard;
        proc::detail::ScopedPagingSwitch paging_guard(t->mem->page_dir);

        sp -= 4u;
        *(uint32_t*)sp = arg;

        sp -= 4u;
        *(uint32_t*)sp = 0u;
    }

    *out_user_esp = sp;
    return 1;
}

static int proc_mem_register_stack_region(proc_mem_t* mem, uint32_t stack_bottom, uint32_t stack_top);

task_t* proc_clone_thread(uint32_t entry, uint32_t arg, uint32_t stack_bottom, uint32_t stack_top) {
    task_t* parent = proc_current();
    if (!parent || !parent->mem || !parent->mem->page_dir) return 0;

    task_t* t = alloc_task();
    if (!t) return 0;

    strlcpy(t->name, parent->name, sizeof(t->name));
    t->cwd_inode = parent->cwd_inode;
    t->terminal = parent->terminal;
    t->term_mode = parent->term_mode;
    t->priority = parent->priority;
    t->stack_bottom = stack_bottom;
    t->stack_top = stack_top;

    {
        kernel::ScopedIrqDisable irq_guard;

        spinlock_acquire(&parent->state_lock);
        spinlock_acquire(&t->state_lock);

        proc::detail::task_set_parent_locked(t, parent);
        proc::detail::task_inherit_process_context_locked(t, parent);

        spinlock_release(&t->state_lock);
        spinlock_release(&parent->state_lock);
    }

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

    proc_task_put(t);
    return t;
}

static void proc_add_mmap_region(task_t* t, vfs_node_t* node, uint32_t vaddr, uint32_t size, uint32_t file_size, uint32_t offset) {
    if (!t || !t->mem) {
        return;
    }

    vma_create(t->mem, vaddr, size, node, offset, file_size, VMA_MAP_PRIVATE);
}

static int proc_mem_register_stack_region(proc_mem_t* mem, uint32_t stack_bottom, uint32_t stack_top) {
    if (!mem || !mem->page_dir) {
        return 0;
    }

    if (stack_top <= stack_bottom) {
        return 0;
    }

    uint32_t start = stack_bottom & ~proc::detail::page_mask;
    uint32_t end_excl = (stack_top + proc::detail::page_mask) & ~proc::detail::page_mask;

    if (end_excl <= start) {
        return 0;
    }

    if (start < proc::detail::user_stack_addr_min || end_excl > proc::detail::user_stack_addr_max) {
        return 0;
    }

    if (vma_has_overlap(mem, start, end_excl)) {
        return 1;
    }

    vma_region_t* region = vma_create(
        mem, start, stack_top - stack_bottom,
        nullptr, 0u, 0u, VMA_MAP_PRIVATE | VMA_MAP_STACK
    );

    return region ? 1 : 0;
}

task_t* proc_spawn_elf(const char* filename, int argc, char** argv) {
    kernel::unique_ptr<vfs_node_t, proc::detail::VfsNodeReleaseDeleter> exec_node(vfs_create_node_from_path(filename));
    if (!exec_node) return 0;

    Elf32_Ehdr header;
    kernel::unique_ptr<Elf32_Phdr, proc::detail::KfreeDeleter<Elf32_Phdr>> phdrs;
    uint32_t max_vaddr = 0;

    int header_read = exec_node->ops->read(
        exec_node.get(),
        0,
        sizeof(Elf32_Ehdr),
        &header
    );
    if (header_read < (int)sizeof(Elf32_Ehdr)) {
        return 0;
    }
    
    if (
        header.e_ident[0] != 0x7F
        || header.e_ident[1] != 'E'
        || header.e_ident[2] != 'L'
        || header.e_ident[3] != 'F'
    ) {
        return 0;
    }

    if (
        header.e_ident[EI_CLASS] != ELFCLASS32
        || header.e_ident[EI_DATA] != ELFDATA2LSB
        || header.e_ident[EI_VERSION] != EV_CURRENT
    ) {
        return 0;
    }

    if (
        header.e_type != ET_EXEC
        || header.e_machine != EM_386
        || header.e_version != EV_CURRENT
    ) {
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

    int phdrs_read = exec_node->ops->read(
        exec_node.get(),
        header.e_phoff,
        (uint32_t)phdr_bytes,
        phdrs.get()
    );
    if (phdrs_read < (int)phdr_bytes) {
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

        if (mem_sz < file_sz) {
            return 0;
        }

        uint32_t end_v = start_v + mem_sz;
        if (end_v < start_v) {
            return 0;
        }

        if (start_v < proc::detail::user_elf_min_vaddr || end_v > proc::detail::user_elf_max_vaddr) {
            return 0;
        }

        uint32_t diff = start_v & proc::detail::page_mask;
        if (file_off < diff) {
            return 0;
        }

        uint64_t file_end = (uint64_t)file_off + (uint64_t)file_sz;
        if (file_end > (uint64_t)exec_node->size) {
            return 0;
        }

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

    task_t* curr = proc_current();
    if (curr) {
        t->cwd_inode = curr->cwd_inode;
        t->terminal = curr->terminal;
        t->term_mode = curr->term_mode;

        {
            kernel::ScopedIrqDisable irq_guard;

            spinlock_acquire(&curr->state_lock);
            spinlock_acquire(&t->state_lock);

            proc::detail::task_set_parent_locked(t, curr);
            proc::detail::task_inherit_process_context_locked(t, curr);

            spinlock_release(&t->state_lock);
            spinlock_release(&curr->state_lock);
        }
    } else {
        t->cwd_inode = 1;

        {
            kernel::ScopedIrqDisable irq_guard;
            spinlock_acquire(&t->state_lock);
            t->parent_pid = 0;
            spinlock_release(&t->state_lock);
        }
    }

    if (curr) {
        fd_table_t* cloned = proc_fd_table_clone(curr->fd_table);
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
        kernel::ScopedIrqDisable irq_guard;
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
    proc_task_put(out);
    return out;
}

void proc_wait(uint32_t pid) {
    task_t* target = proc_find_by_pid(pid);
    if (!target) return;

    task_t* waiter = proc_current();
    if (waiter) {
        waiter->wait_for_pid = pid;
    }

    __sync_fetch_and_add(&target->exit_waiters, 1);
    
    sem_wait(&target->exit_sem);

    if (waiter) {
        waiter->wait_for_pid = 0;
    }
    __sync_fetch_and_sub(&target->exit_waiters, 1);

    proc_task_put(target);
}

int proc_waitpid(uint32_t pid, int* out_status) {
    task_t* target = proc_find_by_pid(pid);
    if (!target) return -1;

    task_t* waiter = proc_current();
    if (waiter) {
        waiter->wait_for_pid = pid;
    }

    __sync_fetch_and_add(&target->exit_waiters, 1);

    sem_wait(&target->exit_sem);

    if (waiter) {
        waiter->wait_for_pid = 0;
    }

    if (out_status) {
        *out_status = target->exit_status;
    }

    __sync_fetch_and_sub(&target->exit_waiters, 1);

    proc_task_put(target);
    return 0;
}

void reaper_task_func(void* arg) {
    (void)arg;
    while (1) {
        task_t* list = nullptr;
        {
            kernel::ScopedIrqDisable irq_guard;
            spinlock_acquire(&proc::detail::g_zombie_lock);

            list = proc::detail::g_zombie_head;
            proc::detail::g_zombie_head = nullptr;

            spinlock_release(&proc::detail::g_zombie_lock);
        }
        task_t* survivors = nullptr;

        while (list) {
            task_t* curr = list;
            list = curr->zombie_next;
            curr->zombie_next = nullptr;

            int still_running = 0;
            for (int i = 0; i < MAX_CPUS; i++) {
                if (cpus[i].current_task == curr || cpus[i].prev_task_during_switch == curr) {
                    still_running = 1;
                    break;
                }
            }

            if (still_running || curr->exit_waiters > 0) {
                curr->zombie_next = survivors;
                survivors = curr;
                continue;
            }

            proc_free_resources(curr);
            proc_task_put(curr);
        }

        if (survivors) {
            task_t* tail = survivors;
            while (tail->zombie_next) {
                tail = tail->zombie_next;
            }

            {
                kernel::ScopedIrqDisable irq_guard;
                spinlock_acquire(&proc::detail::g_zombie_lock);

                tail->zombie_next = proc::detail::g_zombie_head;
                proc::detail::g_zombie_head = survivors;

                spinlock_release(&proc::detail::g_zombie_lock);
            }
        }

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

    bool removed_from_all_tasks = false;
    {
        kernel::PerCpuRwSpinLockNativeWriteSafeGuard guard(proc::detail::task_list_lock);

        if (t->all_tasks_node.next && t->all_tasks_node.prev) {
            dlist_del(&t->all_tasks_node);
            removed_from_all_tasks = true;
        }
        if (removed_from_all_tasks && proc::detail::total_tasks > 0) {
            proc::detail::total_tasks--;
        }
    }

    if (removed_from_all_tasks) {
        proc_task_put(t);
    }

    proc::detail::pid_map_remove(old_pid);

    return t;
}

void proc_sleep_add(task_t* t, uint32_t wake_tick) {
    if (!t) {
        return;
    }

    if (wake_tick == 0u) {
        wake_tick = 1u;
    }

    cpu_t* cpu = cpu_current();
    if (!cpu) {
        return;
    }

    if (proc_change_state(t, TASK_WAITING) != 0) {
        return;
    }

    while (true) {
        const int new_cpu_idx = cpu->index;

        const uint32_t old_wake = __atomic_load_n(&t->wake_tick, __ATOMIC_ACQUIRE);
        const int old_cpu_idx = __atomic_load_n(&t->sleep_cpu, __ATOMIC_ACQUIRE);

        if (old_wake != 0u && (old_cpu_idx < 0 || old_cpu_idx >= MAX_CPUS)) {
            kernel::cpu_relax();
            continue;
        }

        const bool was_sleeping = (
            old_wake != 0u
            && old_cpu_idx >= 0
            && old_cpu_idx < MAX_CPUS
        );

        if (!was_sleeping || old_cpu_idx == new_cpu_idx) {
            kernel::SpinLockNativeSafeGuard guard(cpu->sleep_lock);

            if (__atomic_load_n(&t->wake_tick, __ATOMIC_ACQUIRE) != old_wake
                || (was_sleeping && __atomic_load_n(&t->sleep_cpu, __ATOMIC_ACQUIRE) != old_cpu_idx)) {
                continue;
            }

            if (__atomic_load_n(&t->wake_tick, __ATOMIC_ACQUIRE) != 0u) {
                proc::detail::cpu_sleep_remove_locked(cpu, t);
            }

            __atomic_store_n(&t->sleep_cpu, new_cpu_idx, __ATOMIC_RELEASE);
            __atomic_store_n(&t->wake_tick, wake_tick, __ATOMIC_RELEASE);

            proc::detail::cpu_sleep_insert_locked(cpu, t);
            break;
        }

        cpu_t* old_cpu = &cpus[old_cpu_idx];

        cpu_t* first = old_cpu;
        cpu_t* second = cpu;
        if (first->index > second->index) {
            first = cpu;
            second = old_cpu;
        }

        kernel::SpinLockNativeSafeGuard first_guard(first->sleep_lock);
        kernel::SpinLockNativeSafeGuard second_guard(second->sleep_lock);

        if (__atomic_load_n(&t->wake_tick, __ATOMIC_ACQUIRE) != old_wake
            || __atomic_load_n(&t->sleep_cpu, __ATOMIC_ACQUIRE) != old_cpu_idx) {
            continue;
        }

        proc::detail::cpu_sleep_remove_locked(old_cpu, t);

        __atomic_store_n(&t->sleep_cpu, new_cpu_idx, __ATOMIC_RELEASE);
        __atomic_store_n(&t->wake_tick, wake_tick, __ATOMIC_RELEASE);

        proc::detail::cpu_sleep_insert_locked(cpu, t);
        break;
    }

    sched_yield();
}

void proc_usleep(uint32_t us) {
    task_t* curr = proc_current();
    if (!curr) return;

    uint32_t ticks = (uint32_t)(((uint64_t)us * KERNEL_TIMER_HZ) / 1000000ull);
    if (ticks == 0) {
        ticks = 1;
    }

    uint32_t target = timer_ticks + ticks;
    proc_sleep_add(curr, target);
}

void proc_check_sleepers(uint32_t current_tick) {
    cpu_t* cpu = cpu_current();
    if (!cpu) {
        return;
    }

    if (current_tick < cpu->sleep_next_wake_tick) {
        return;
    }

    if (!spinlock_try_acquire(&cpu->sleep_lock)) {
        return;
    }

    while (cpu->sleep_leftmost) {
        task_t* t = cpu->sleep_leftmost;
        if (!t) {
            break;
        }

        if (__atomic_load_n(&t->wake_tick, __ATOMIC_ACQUIRE) > current_tick) {
            break;
        }

        proc::detail::cpu_sleep_remove_locked(cpu, t);

        if (proc_change_state(t, TASK_RUNNABLE) != 0) {
            __atomic_store_n(&t->wake_tick, 0u, __ATOMIC_RELEASE);
            __atomic_store_n(&t->sleep_cpu, -1, __ATOMIC_RELEASE);
            continue;
        }

        __atomic_store_n(&t->wake_tick, 0u, __ATOMIC_RELEASE);
        __atomic_store_n(&t->sleep_cpu, -1, __ATOMIC_RELEASE);

        sem_remove_task(t);

        sched_add(t);
    }

    spinlock_release(&cpu->sleep_lock);
}

void proc_sleep_remove(task_t* t) {
    if (!t) {
        return;
    }

    while (true) {
        if (__atomic_load_n(&t->wake_tick, __ATOMIC_ACQUIRE) == 0u) {
            return;
        }

        const int cpu_idx = __atomic_load_n(&t->sleep_cpu, __ATOMIC_ACQUIRE);
        if (cpu_idx < 0 || cpu_idx >= MAX_CPUS) {
            kernel::cpu_relax();
            continue;
        }

        cpu_t* cpu = &cpus[cpu_idx];
        kernel::SpinLockNativeSafeGuard guard(cpu->sleep_lock);

        if (__atomic_load_n(&t->sleep_cpu, __ATOMIC_ACQUIRE) != cpu_idx) {
            continue;
        }

        if (__atomic_load_n(&t->wake_tick, __ATOMIC_ACQUIRE) != 0u) {
            proc::detail::cpu_sleep_remove_locked(cpu, t);
            __atomic_store_n(&t->wake_tick, 0u, __ATOMIC_RELEASE);
            __atomic_store_n(&t->sleep_cpu, -1, __ATOMIC_RELEASE);
        }

        return;
    }
}

void proc_wake(task_t* t) {
    if (!t) return;

    if (t->state == TASK_ZOMBIE || t->state == TASK_UNUSED) {
        return;
    }

    proc_sleep_remove(t);

    if (t->blocked_on_sem) return;

    if (proc_change_state(t, TASK_RUNNABLE) == 0) {
        sched_add(t);
    }
}

int proc_change_state(task_t* t, task_state_t new_state) {
    if (!t) {
        return -1;
    }

    uint32_t flags = spinlock_acquire_safe(&t->state_lock);

    const task_state_t old_state = t->state;
    if (old_state == TASK_UNUSED) {
        spinlock_release_safe(&t->state_lock, flags);
        return -1;
    }
    if (old_state == TASK_ZOMBIE && new_state != TASK_ZOMBIE) {
        spinlock_release_safe(&t->state_lock, flags);
        return -1;
    }

    bool ok = true;
    switch (new_state) {
        case TASK_UNUSED:
            ok = false;
            break;

        case TASK_ZOMBIE:
            ok = true;
            break;

        case TASK_WAITING:
            ok = old_state == TASK_RUNNING
                || old_state == TASK_RUNNABLE
                || old_state == TASK_WAITING;
            break;

        case TASK_RUNNABLE:
            ok = old_state == TASK_WAITING
                || old_state == TASK_RUNNING
                || old_state == TASK_RUNNABLE;
            break;

        case TASK_RUNNING:
            ok = old_state == TASK_RUNNABLE
                || old_state == TASK_RUNNING
                || old_state == TASK_WAITING;
            break;

        case TASK_STOPPED:
            ok = true;
            break;
    }

    if (ok) {
        t->state = new_state;
    }

    spinlock_release_safe(&t->state_lock, flags);
    return ok ? 0 : -1;
}
