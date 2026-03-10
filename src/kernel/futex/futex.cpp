// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 Yula1234

#include <kernel/futex/futex.h>

#include <kernel/panic.h>

#include <kernel/proc.h>
#include <kernel/sched.h>

#include <kernel/uaccess/uaccess.h>

#include <lib/cpp/lock_guard.h>
#include <lib/cpp/intrusive_ref.h>

#include <lib/hash_map.h>
#include <lib/cpp/new.h>

#include <stdint.h>

namespace {

struct futex_entry_t {
    uint32_t key;
    uint32_t refs;

    kernel::SpinLock lock;
    dlist_head_t wait_list;

    bool retain();
    void release();
};

static __cacheline_aligned kernel::SpinLock futex_table_lock;
static __attribute__((unused)) uint8_t futex_table_lock_pad[HAL_CACHELINE_SIZE - sizeof(kernel::SpinLock)];

static HashMap<uint32_t, futex_entry_t*> futex_map;

static kernel::IntrusiveRef<futex_entry_t> futex_acquire_entry(uint32_t key, bool create) {
    kernel::SpinLockSafeGuard guard(futex_table_lock);

    futex_entry_t* entry = nullptr;

    if (futex_map.try_get(key, entry)) {
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

    dlist_init(&entry->wait_list);

    const bool inserted = futex_map.insert_unique(key, entry);
    if (!inserted) {
        delete entry;
        return {};
    }

    return kernel::IntrusiveRef<futex_entry_t>::adopt(entry);
}

static bool futex_entry_is_unused(futex_entry_t& entry) {
    kernel::SpinLockSafeGuard guard(entry.lock);

    const bool unused = dlist_empty(&entry.wait_list);

    return unused;
}

bool futex_entry_t::retain() {
    kernel::SpinLockSafeGuard guard(futex_table_lock);

    if (refs == 0u) {
        return false;
    }

    futex_entry_t* current = nullptr;
    if (!futex_map.try_get(key, current) || current != this) {
        return false;
    }

    refs++;
    return true;
}

void futex_entry_t::release() {
    bool maybe_free = false;

    {
        kernel::SpinLockSafeGuard guard(futex_table_lock);

        if (refs == 0u) {
            panic("FUTEX: entry ref underflow");
        }

        refs--;
        maybe_free = (refs == 0u);
    }

    if (!maybe_free) {
        return;
    }

    if (!futex_entry_is_unused(*this)) {
        return;
    }

    futex_entry_t* removed = nullptr;

    {
        kernel::SpinLockSafeGuard guard(futex_table_lock);

        futex_entry_t* current = nullptr;
        if (!futex_map.try_get(key, current) || current != this) {
            return;
        }

        if (refs != 0u) {
            return;
        }

        if (!futex_entry_is_unused(*this)) {
            return;
        }

        (void)futex_map.remove_and_get(key, removed);
    }

    if (removed) {
        delete removed;
    }
}

static int futex_do_wait(futex_entry_t* entry, volatile const uint32_t* uaddr, uint32_t expected) {
    if (!entry || !uaddr) {
        return -1;
    }

    for (;;) {
        uaccess_prefault_user_read((const void*)uaddr, 4u);

        uint32_t v = *(volatile const uint32_t*)uaddr;
        if (v != expected) {
            return 0;
        }

        {
            kernel::SpinLockSafeGuard guard(entry->lock);

            v = *(volatile const uint32_t*)uaddr;
            if (v != expected) {
                return 0;
            }

            task_t* curr = proc_current();
            if (!curr) {
                return -1;
            }

            curr->blocked_on_sem = (void*)entry;

            dlist_add_tail(&curr->sem_node, &entry->wait_list);
            curr->state = TASK_WAITING;
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

        while (woken < max_wake && !dlist_empty(&entry->wait_list)) {
            task_t* t = container_of(entry->wait_list.next, task_t, sem_node);

            dlist_del(&t->sem_node);
            t->sem_node.next = 0;
            t->sem_node.prev = 0;

            t->blocked_on_sem = 0;

            if (t->state != TASK_ZOMBIE) {
                t->state = TASK_RUNNABLE;

                sched_add(t);
            }

            woken++;
        }
    }

    return (int)woken;
}

}

extern "C" int futex_wait(uint32_t key, volatile const uint32_t* uaddr, uint32_t expected) {
    kernel::IntrusiveRef<futex_entry_t> entry_ref = futex_acquire_entry(key, true);
    if (!entry_ref) {
        return -1;
    }

    const int rc = futex_do_wait(entry_ref.get(), uaddr, expected);

    return rc;
}

extern "C" int futex_wake(uint32_t key, uint32_t max_wake) {
    kernel::IntrusiveRef<futex_entry_t> entry_ref = futex_acquire_entry(key, false);
    if (!entry_ref) {
        return 0;
    }

    const int rc = futex_do_wake(entry_ref.get(), max_wake);

    return rc;
}
