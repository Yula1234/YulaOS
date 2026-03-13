// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 Yula1234

#include <kernel/uaccess/uaccess.h>

#include <kernel/futex/futex.h>

#include <kernel/panic.h>
#include <kernel/sched.h>
#include <kernel/proc.h>

#include <lib/cpp/intrusive_ref.h>
#include <lib/cpp/lock_guard.h>
#include <lib/cpp/dlist.h>
#include <lib/cpp/new.h>

#include <lib/hash_map.h>

#include <stdint.h>

namespace {

class FutexTable;

struct futex_entry_t {
    uint32_t key;
    uint32_t refs;

    kernel::SpinLock lock;
    dlist_head_t wait_list;

    FutexTable* table;

    void release();
};

class FutexTable {
public:
    kernel::IntrusiveRef<futex_entry_t> acquire_entry(uint32_t key, bool create) {
        kernel::SpinLockSafeGuard guard(lock_);

        futex_entry_t* entry = nullptr;

        if (map_.try_get(key, entry)) {
            if (!entry->table) {
                entry->table = this;
            } else if (entry->table != this) {
                panic("FUTEX: entry table mismatch");
            }

            entry->refs++;

            return kernel::IntrusiveRef<futex_entry_t>::adopt(entry);
        }

        if (!create) {
            return {};
        }

        entry = new (kernel::nothrow) futex_entry_t();

        if (!entry) {
            return {};
        }

        entry->key = key;
        entry->refs = 1u;
        entry->table = this;

        dlist_init(&entry->wait_list);

        const bool inserted = map_.insert_unique(key, entry);

        if (!inserted) {
            delete entry;

            return {};
        }

        return kernel::IntrusiveRef<futex_entry_t>::adopt(entry);
    }

    void release_entry(futex_entry_t& entry) {
        bool maybe_free = false;

        {
            kernel::SpinLockSafeGuard guard(lock_);

            if (entry.refs == 0u) {
                panic("FUTEX: entry ref underflow");
            }

            entry.refs--;

            maybe_free = (entry.refs == 0u);
        }

        if (!maybe_free) {
            return;
        }

        if (!entry_is_unused(entry)) {
            return;
        }

        futex_entry_t* removed = nullptr;

        {
            kernel::SpinLockSafeGuard guard(lock_);

            futex_entry_t* current = nullptr;

            if (!map_.try_get(entry.key, current) || current != &entry) {
                return;
            }

            if (entry.refs != 0u) {
                return;
            }

            if (!entry_is_unused(entry)) {
                return;
            }

            (void)map_.remove_and_get(entry.key, removed);
        }

        if (removed) {
            delete removed;
        }
    }

private:
    static bool entry_is_unused(futex_entry_t& entry) {
        kernel::SpinLockSafeGuard guard(entry.lock);

        const bool unused = dlist_empty(&entry.wait_list);

        return unused;
    }

    kernel::SpinLock lock_;
    HashMap<uint32_t, futex_entry_t*> map_;
};

static __cacheline_aligned FutexTable futex_table;

void futex_entry_t::release() {
    if (!table) {
        panic("FUTEX: entry missing table");
    }

    table->release_entry(*this);
}

static int futex_do_wait(futex_entry_t* entry, volatile const uint32_t* uaddr, uint32_t expected) {
    if (!entry || !uaddr) {
        return -1;
    }

    for (;;) {
        task_t* curr = proc_current();

        if (curr && curr->pending_signals != 0) {
            return -2;
        }

        uaccess_prefault_user_read((const void*)uaddr, 4u);

        uint32_t v = 0u;
        if (uaccess_copy_from_user(&v, (const void*)uaddr, sizeof(v)) != 0) {
            return -1;
        }

        if (v != expected) {
            return 0;
        }

        {
            kernel::SpinLockSafeGuard guard(entry->lock);

            if (!curr) {
                return -1;
            }

            curr->blocked_on_sem = (void*)entry;
            curr->blocked_kind = TASK_BLOCK_FUTEX;

            dlist_add_tail(&curr->sem_node, &entry->wait_list);

            curr->state = TASK_WAITING;
        }

        v = 0u;
        if (uaccess_copy_from_user(&v, (const void*)uaddr, sizeof(v)) != 0) {
            return -1;
        }

        if (v != expected) {
            kernel::SpinLockSafeGuard guard(entry->lock);

            if (curr && curr->blocked_on_sem == entry) {
                if (dlist_unlink_consistent(&curr->sem_node)) {
                    curr->blocked_on_sem = 0;
                    curr->blocked_kind = TASK_BLOCK_NONE;
                    curr->state = TASK_RUNNING;
                }
            }

            return 0;
        }

        sched_yield();
    }
}

static int futex_do_wake(futex_entry_t* entry, uint32_t max_wake) {
    if (!entry || max_wake == 0u) {
        return 0;
    }

    uint32_t woken = 0;

    {
        kernel::SpinLockSafeGuard guard(entry->lock);

        kernel::CDBLinkedListView<task_t, &task_t::sem_node> waiters(entry->wait_list);

        while (woken < max_wake && !waiters.empty()) {
            task_t& t = waiters.front();

            if (!dlist_unlink_consistent(&t.sem_node)) {
                panic("FUTEX: waiter unlink failed");
            }

            t.blocked_on_sem = 0;
            t.blocked_kind = TASK_BLOCK_NONE;

            if (t.state != TASK_ZOMBIE) {
                t.state = TASK_RUNNABLE;

                sched_add(&t);
            }

            woken++;
        }
    }

    return (int)woken;
}

}

extern "C" int futex_wait(uint32_t key, volatile const uint32_t* uaddr, uint32_t expected) {
    kernel::IntrusiveRef<futex_entry_t> entry_ref = futex_table.acquire_entry(key, true);

    if (!entry_ref) {
        return -1;
    }

    const int rc = futex_do_wait(entry_ref.get(), uaddr, expected);

    return rc;
}

extern "C" int futex_wake(uint32_t key, uint32_t max_wake) {
    kernel::IntrusiveRef<futex_entry_t> entry_ref = futex_table.acquire_entry(key, false);

    if (!entry_ref) {
        return 0;
    }

    const int rc = futex_do_wake(entry_ref.get(), max_wake);

    return rc;
}
