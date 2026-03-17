#include "poll_waitq.h"

#include <arch/i386/paging.h>

#include <kernel/panic.h>
#include <kernel/proc.h>

#include <lib/cpp/atomic.h>
#include <lib/cpp/dlist.h>
#include <lib/cpp/lock_guard.h>

#include <stdint.h>

namespace {

static_assert(
    sizeof(void*) == sizeof(uint32_t),
    "poll_waitq assumes 32-bit pointers"
);

static inline bool poll_ptr_mapped(const void* p) {
    if (!p) {
        return false;
    }

    const uint32_t v = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(p));

    auto dir_mapped = [&](uint32_t* dir) {
        if (!dir) {
            return false;
        }

        const uint32_t pde = dir[v >> 22];
        if ((pde & PTE_PRESENT) == 0u) {
            return false;
        }

        if ((pde & (1u << 7)) != 0u) {
            return true;
        }

        return paging_get_present_pte(dir, v, nullptr) != 0;
    };

    uint32_t* active_dir = paging_get_dir();
    if (dir_mapped(active_dir)) {
        return true;
    }

    if (dir_mapped(kernel_page_directory)) {
        return true;
    }

    return false;
}

static inline int poll_dlist_node_valid(const dlist_head_t* node) {
    return poll_ptr_mapped(node) ? 1 : 0;
}

static inline void poll_on_corrupt(const char* msg) {
    panic(msg);
}

class WaiterLink {
public:
    explicit WaiterLink(poll_waiter_t& w)
        : w_(w) {
    }

    task_t* task_ptr() const {
        return w_.task;
    }

    poll_waitq_t* queue_ptr() const {
        return w_.q;
    }

    bool has_task() const {
        return task_ptr() != nullptr;
    }

    bool has_queue() const {
        return queue_ptr() != nullptr;
    }

    bool is_attached() const {
        return has_task() && has_queue();
    }

    bool is_clean_for_register() const {
        return !has_task()
            && !has_queue()
            && !dlist_node_linked(&w_.q_node)
            && !dlist_node_linked(&w_.task_node);
    }

    bool attach(task_t& task, poll_waitq_t& q) {
        if (!proc_task_retain(&task)) {
            return false;
        }

        w_.task = &task;
        w_.q = &q;

        dlist_add_tail(&w_.q_node, &q.waiters);
        dlist_add_tail(&w_.task_node, &task.poll_waiters);

        return true;
    }

    void detach_from_queue_only(poll_waitq_t& q) {
        detach_from_queue(q);
        reset_queue_node();
        clear_queue();
    }

    void detach_from_task_only(task_t& task) {
        detach_from_task_list(task);

        clear_task_and_queue();
    }

    void detach_from_task_list_only(task_t& task) {
        detach_from_task_list(task);
        reset_task_node();
    }

    void clear_task_ptr_no_put() {
        clear_task_no_put();
    }

    void detach_everywhere(task_t& task, poll_waitq_t& q) {
        detach_from_queue(q);
        detach_from_task_list(task);

        clear_task_and_queue();
    }

    void clear_if_no_task() {
        if (task_ptr() != nullptr) {
            return;
        }

        clear_task_and_queue();
    }

private:
    void reset_queue_node() {
        w_.q_node.next = nullptr;
        w_.q_node.prev = nullptr;
    }

    void reset_task_node() {
        w_.task_node.next = nullptr;
        w_.task_node.prev = nullptr;
    }

    void clear_task() {
        if (w_.task) {
            proc_task_put(w_.task);
        }
        w_.task = nullptr;
    }

    void clear_task_no_put() {
        w_.task = nullptr;
    }

    void clear_queue() {
        w_.q = nullptr;
    }

    void clear_task_and_queue() {
        clear_task();
        clear_queue();

        reset_queue_node();
        reset_task_node();
    }

    void detach_from_task_list(task_t& task) {
        (void)dlist_remove_node_if_present_checked(
            &task.poll_waiters,
            &w_.task_node,
            poll_dlist_node_valid,
            poll_on_corrupt
        );
    }

    void detach_from_queue(poll_waitq_t& q) {
        (void)dlist_remove_node_if_present_checked(
            &q.waiters,
            &w_.q_node,
            poll_dlist_node_valid,
            poll_on_corrupt
        );
    }

    poll_waiter_t& w_;
};

class TaskPollList {
public:
    explicit TaskPollList(task_t& task)
        : task_(task) {
    }

    task_t& task() const {
        return task_;
    }

    spinlock_t& lock_native() const {
        return task_.poll_lock;
    }

    dlist_head_t& waiters() const {
        return task_.poll_waiters;
    }

private:
    task_t& task_;
};

class PollWaitQueue {
public:
    explicit PollWaitQueue(poll_waitq_t& q)
        : q_(q) {
    }

    poll_waitq_t& queue() const {
        return q_;
    }

    spinlock_t& lock_native() const {
        return q_.lock;
    }

    dlist_head_t& waiters() const {
        return q_.waiters;
    }

private:
    poll_waitq_t& q_;
};

using PollWaitersByQueue = kernel::CDBLinkedListView<poll_waiter_t, &poll_waiter_t::q_node>;
using PollWaitersByTask = kernel::CDBLinkedListView<poll_waiter_t, &poll_waiter_t::task_node>;

static inline int register_waiter(poll_waitq_t& q, poll_waiter_t& w, task_t& task) {
    WaiterLink link(w);

    TaskPollList task_list(task);

    [[maybe_unused]] kernel::SpinLockNativeSafeGuard task_guard(task_list.lock_native());

    if (!link.is_clean_for_register()) {
        return -1;
    }

    PollWaitQueue waitq(q);
    [[maybe_unused]] kernel::SpinLockNativeSafeGuard q_guard(waitq.lock_native());

    if (!link.attach(task, q)) {
        return -1;
    }

    return 0;
}

static inline void unregister_waiter(poll_waiter_t& w) {
    WaiterLink link(w);

    if (!poll_ptr_mapped(&w)) {
        panic("POLL: waiter unmapped");
    }

    task_t* task = link.task_ptr();
    if (!task) {
        link.clear_if_no_task();

        return;
    }

    if (!poll_ptr_mapped(task)) {
        panic("POLL: waiter->task unmapped");
    }

    TaskPollList task_list(*task);

    [[maybe_unused]] kernel::SpinLockNativeSafeGuard task_guard(task_list.lock_native());

    if (link.task_ptr() != task) {
        poll_waitq_t* q = link.queue_ptr();
        if (q && poll_ptr_mapped(q)) {
            PollWaitQueue waitq(*q);
            [[maybe_unused]] kernel::SpinLockNativeSafeGuard q_guard(waitq.lock_native());

            if (link.queue_ptr() == q) {
                link.detach_from_queue_only(*q);
            }
        }
        return;
    }

    poll_waitq_t* q = link.queue_ptr();
    if (q && !poll_ptr_mapped(q)) {
        link.detach_from_task_only(*task);
        return;
    }

    if (!q) {
        link.detach_from_task_only(*task);
        return;
    }

    PollWaitQueue waitq(*q);
    [[maybe_unused]] kernel::SpinLockNativeSafeGuard q_guard(waitq.lock_native());

    if (link.queue_ptr() != q) {
        if (link.queue_ptr() == nullptr) {
            link.detach_from_task_only(*task);
        }
        return;
    }

    link.detach_everywhere(*task, *q);
}

static inline void wake_all(poll_waitq_t& q) {
    PollWaitQueue waitq(q);
    [[maybe_unused]] kernel::SpinLockNativeGuard q_guard(waitq.lock_native());

    for (dlist_head_t* it = q.waiters.next; it && it != &q.waiters; it = it->next) {
        poll_waiter_t* w = container_of(it, poll_waiter_t, q_node);
        if (!w) {
            continue;
        }

        task_t* task = w->task;
        if (!task) {
            continue;
        }

        proc_wake(task);
    }
}

static inline void detach_all(poll_waitq_t& q) {
    for (;;) {
        spinlock_acquire(&q.lock);

        if (dlist_empty(&q.waiters)) {
            spinlock_release(&q.lock);
            return;
        }

        dlist_head_t* it = q.waiters.next;
        if (!it || it == &q.waiters) {
            spinlock_release(&q.lock);
            return;
        }

        poll_waiter_t* w = container_of(it, poll_waiter_t, q_node);
        if (!w) {
            dlist_del(it);
            it->next = nullptr;
            it->prev = nullptr;

            spinlock_release(&q.lock);
            continue;
        }

        task_t* task = w->task;
        if (!task) {
            w->q = nullptr;

            dlist_del(&w->q_node);
            w->q_node.next = nullptr;
            w->q_node.prev = nullptr;

            spinlock_release(&q.lock);
            continue;
        }

        if (!spinlock_try_acquire(&task->poll_lock)) {
            spinlock_release(&q.lock);
            kernel::cpu_relax();
            continue;
        }

        w->q = nullptr;

        if (w->task == task) {
            if (w->task_node.next && w->task_node.prev) {
                dlist_del(&w->task_node);
                w->task_node.next = nullptr;
                w->task_node.prev = nullptr;
            }
        }

        dlist_del(&w->q_node);
        w->q_node.next = nullptr;
        w->q_node.prev = nullptr;

        spinlock_release(&task->poll_lock);

        proc_wake(task);

        spinlock_release(&q.lock);
    }
}

static inline void task_cleanup(task_t& task) {
    TaskPollList task_list(task);

    [[maybe_unused]] kernel::SpinLockNativeSafeGuard task_guard(task_list.lock_native());

    PollWaitersByTask waiters(task.poll_waiters);

    while (!waiters.empty()) {
        poll_waiter_t& w = waiters.front();
        WaiterLink link(w);

        poll_waitq_t* q = link.queue_ptr();

        if (!q || !poll_ptr_mapped(q)) {
            link.detach_from_task_only(task);
            continue;
        }

        PollWaitQueue waitq(*q);
        [[maybe_unused]] kernel::SpinLockNativeSafeGuard q_guard(waitq.lock_native());

        if (link.queue_ptr() != q) {
            if (link.queue_ptr() == nullptr) {
                link.detach_from_task_only(task);
            }
            continue;
        }

        link.detach_everywhere(task, *q);
    }
}

}

extern "C" {

void poll_waitq_init(poll_waitq_t* q) {
    if (!q) {
        return;
    }

    spinlock_init(&q->lock);

    dlist_init(&q->waiters);
}

int poll_waitq_register(poll_waitq_t* q, poll_waiter_t* w, struct task* task) {
    if (!q || !w || !task) {
        return -1;
    }

    return register_waiter(*q, *w, *task);
}

void poll_waitq_unregister(poll_waiter_t* w) {
    if (!w) {
        return;
    }

    unregister_waiter(*w);
}

void poll_waitq_wake_all(poll_waitq_t* q) {
    if (!q) {
        return;
    }

    wake_all(*q);
}

void poll_waitq_detach_all(poll_waitq_t* q) {
    if (!q) {
        return;
    }

    detach_all(*q);
}

void poll_task_cleanup(struct task* task) {
    if (!task) {
        return;
    }

    task_cleanup(*task);
}

}
