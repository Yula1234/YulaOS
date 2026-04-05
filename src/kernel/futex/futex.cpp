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

struct FutexShard;

struct futex_entry_t {
    uint32_t key;
    uint32_t refs;

    kernel::SpinLock lock;
    dlist_head_t wait_list;

    FutexShard* shard;

    void release();
};

struct FutexShard {
    kernel::SpinLock lock;
    
    HashMap<uint32_t, futex_entry_t*> map;
} __cacheline_aligned;

class FutexTable {
public:
    kernel::IntrusiveRef<futex_entry_t> acquire_entry(uint32_t key, bool create) {
        FutexShard& shard = shard_for(key);

        kernel::SpinLockSafeGuard guard(shard.lock);

        futex_entry_t* entry = nullptr;

        if (shard.map.try_get(key, entry)) {
            if (!entry->shard) {
                entry->shard = &shard;
            } else if (entry->shard != &shard) {
                panic("FUTEX: entry shard mismatch");
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
        entry->shard = &shard;

        dlist_init(&entry->wait_list);

        const bool inserted = shard.map.insert_unique(key, entry);

        if (!inserted) {
            delete entry;

            return {};
        }

        return kernel::IntrusiveRef<futex_entry_t>::adopt(entry);
    }

    void release_entry(futex_entry_t& entry) {
        bool maybe_free = false;

        FutexShard* shard = entry.shard;
        if (!shard) {
            panic("FUTEX: entry missing shard");
        }

        {
            kernel::SpinLockSafeGuard guard(shard->lock);

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
            kernel::SpinLockSafeGuard guard(shard->lock);

            futex_entry_t* current = nullptr;

            if (!shard->map.try_get(entry.key, current) || current != &entry) {
                return;
            }

            if (entry.refs != 0u) {
                return;
            }

            if (!entry_is_unused(entry)) {
                return;
            }

            (void)shard->map.remove_and_get(entry.key, removed);
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

    static constexpr uint32_t kShards = 64u;

    static FutexShard& shard_for(uint32_t key) {
        return shards_[key & (kShards - 1u)];
    }

    static FutexShard shards_[kShards];
};

static __cacheline_aligned FutexTable futex_table;

FutexShard FutexTable::shards_[FutexTable::kShards];

void futex_entry_t::release() {
    if (!shard) {
        panic("FUTEX: entry missing shard");
    }

    futex_table.release_entry(*this);
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

            if (proc_change_state(curr, TASK_WAITING) != 0) {
                dlist_del(&curr->sem_node);

                curr->sem_node.next = nullptr;
                curr->sem_node.prev = nullptr;
                curr->blocked_on_sem = 0;
                curr->blocked_kind = TASK_BLOCK_NONE;
            }
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
                    (void)proc_change_state(curr, TASK_RUNNING);
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

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstringop-overflow"

        while (woken < max_wake && !waiters.empty()) {
            task_t& t = waiters.front();

            __atomic_fetch_add(&t.in_transit, 1u, __ATOMIC_ACQUIRE);

            if (!dlist_unlink_consistent(&t.sem_node)) {
                panic("FUTEX: waiter unlink failed");
            }

            t.blocked_on_sem = 0;
            t.blocked_kind = TASK_BLOCK_NONE;

            if (proc_change_state(&t, TASK_RUNNABLE) == 0) {
                sched_add(&t);
            }

            __atomic_fetch_sub(&t.in_transit, 1u, __ATOMIC_RELEASE);

            woken++;
        }

#pragma GCC diagnostic pop

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

extern "C" void futex_remove_task(struct task* t) {
    if (!t || t->blocked_kind != TASK_BLOCK_FUTEX || !t->blocked_on_sem) {
        return;
    }

    auto* entry = static_cast<futex_entry_t*>(t->blocked_on_sem);

    {
        kernel::SpinLockSafeGuard guard(entry->lock);

        if (t->blocked_on_sem != entry || t->blocked_kind != TASK_BLOCK_FUTEX) {
            return;
        }

        if (t->sem_node.next && t->sem_node.prev) {
            dlist_del(&t->sem_node);
            t->sem_node.next = nullptr;
            t->sem_node.prev = nullptr;
        }

        t->blocked_on_sem = nullptr;
        t->blocked_kind = TASK_BLOCK_NONE;
    }
}
