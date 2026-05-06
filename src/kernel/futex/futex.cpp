// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 Yula1234

#include <kernel/uaccess/uaccess.h>

#include <kernel/futex/futex.h>

#include <kernel/panic.h>
#include <kernel/sched.h>
#include <kernel/proc.h>

#include <kernel/waitq/waitqueue.h>

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
    waitqueue_t waitq;

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

        waitqueue_init(&entry->waitq, entry, TASK_BLOCK_FUTEX);

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

        const bool unused = dlist_empty(&entry.waitq.waiters);

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

            (void)waitqueue_wait_prepare_locked(&entry->waitq, curr);
        }

        v = 0u;
        if (uaccess_copy_from_user(&v, (const void*)uaddr, sizeof(v)) != 0) {
            kernel::SpinLockSafeGuard guard(entry->lock);

            if (curr) {
                waitqueue_wait_cancel_locked(&entry->waitq, curr);
            }

            if (curr) {
                (void)proc_change_state(curr, TASK_RUNNING);
            }
            
            return -1;
        }

        if (v != expected) {
            kernel::SpinLockSafeGuard guard(entry->lock);

            if (curr) {
                waitqueue_wait_cancel_locked(&entry->waitq, curr);
            }
            
            if (curr) {
                (void)proc_change_state(curr, TASK_RUNNING);
            }

            return 0;
        }

        sched_yield();

        futex_remove_task(curr);
    }
}

static int futex_do_wake(futex_entry_t* entry, uint32_t max_wake) {
    if (!entry || max_wake == 0u) {
        return 0;
    }

    uint32_t woken = 0;

    {
        kernel::SpinLockSafeGuard guard(entry->lock);

        while (woken < max_wake) {
            task_t* t = waitqueue_dequeue_locked(&entry->waitq);
            if (!t) {
                break;
            }

            if (t->state == TASK_ZOMBIE || t->state == TASK_UNUSED) {
                continue;
            }

            waitqueue_wake_task(t);
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

extern "C" void futex_remove_task(struct task* t) {
    if (!t) {
        return;
    }

    void* raw_sem = __atomic_load_n(&t->blocked_on_sem, __ATOMIC_ACQUIRE);
    if (!raw_sem || t->blocked_kind != TASK_BLOCK_FUTEX) {
        return;
    }

    auto* entry = static_cast<futex_entry_t*>(raw_sem);

    kernel::SpinLockSafeGuard guard(entry->lock);

    waitqueue_wait_cancel_locked(&entry->waitq, t);
}