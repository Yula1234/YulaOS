// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 Yula1234

#include <kernel/futex/futex.h>

#include <kernel/panic.h>

#include <kernel/proc.h>
#include <kernel/sched.h>

#include <kernel/uaccess/uaccess.h>

#include <hal/lock.h>

#include <lib/hash_map.h>
#include <lib/cpp/new.h>

#include <stdint.h>

namespace {

typedef struct {
    uint32_t key;
    uint32_t refs;

    spinlock_t lock;
    dlist_head_t wait_list;
} futex_entry_t;

static __cacheline_aligned spinlock_t futex_table_lock;
static __attribute__((unused)) uint8_t futex_table_lock_pad[HAL_CACHELINE_SIZE - sizeof(spinlock_t)];

static HashMap<uint32_t, futex_entry_t*> futex_map;

static futex_entry_t* futex_acquire_entry(uint32_t key, bool create) {
    const uint32_t flags = spinlock_acquire_safe(&futex_table_lock);

    futex_entry_t* entry = nullptr;

    if (futex_map.try_get(key, entry)) {
        entry->refs++;

        spinlock_release_safe(&futex_table_lock, flags);

        return entry;
    }

    if (!create) {
        spinlock_release_safe(&futex_table_lock, flags);

        return nullptr;
    }

    entry = new (kernel::nothrow) futex_entry_t();
    if (!entry) {
        spinlock_release_safe(&futex_table_lock, flags);

        return nullptr;
    }

    entry->key = key;
    entry->refs = 1u;

    spinlock_init(&entry->lock);
    dlist_init(&entry->wait_list);

    const bool inserted = futex_map.insert_unique(key, entry);
    if (!inserted) {
        delete entry;

        spinlock_release_safe(&futex_table_lock, flags);

        return nullptr;
    }

    spinlock_release_safe(&futex_table_lock, flags);

    return entry;
}

static bool futex_entry_is_unused(futex_entry_t& entry) {
    const uint32_t sflags = spinlock_acquire_safe(&entry.lock);
    const bool unused = dlist_empty(&entry.wait_list);
    spinlock_release_safe(&entry.lock, sflags);

    return unused;
}

static void futex_release_entry(futex_entry_t* entry) {
    if (!entry) {
        return;
    }

    bool maybe_free = false;

    {
        const uint32_t flags = spinlock_acquire_safe(&futex_table_lock);

        if (entry->refs == 0u) {
            panic("FUTEX: entry ref underflow");
        }

        entry->refs--;
        maybe_free = (entry->refs == 0u);

        spinlock_release_safe(&futex_table_lock, flags);
    }

    if (!maybe_free) {
        return;
    }

    if (!futex_entry_is_unused(*entry)) {
        return;
    }

    const uint32_t flags = spinlock_acquire_safe(&futex_table_lock);

    futex_entry_t* current = nullptr;
    const bool still_present = futex_map.try_get(entry->key, current);
    if (!still_present || current != entry) {
        spinlock_release_safe(&futex_table_lock, flags);

        return;
    }

    if (entry->refs != 0u) {
        spinlock_release_safe(&futex_table_lock, flags);

        return;
    }

    if (!futex_entry_is_unused(*entry)) {
        spinlock_release_safe(&futex_table_lock, flags);

        return;
    }

    futex_entry_t* removed = nullptr;
    (void)futex_map.remove_and_get(entry->key, removed);

    spinlock_release_safe(&futex_table_lock, flags);

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

        const uint32_t flags = spinlock_acquire_safe(&entry->lock);

        v = *(volatile const uint32_t*)uaddr;
        if (v != expected) {
            spinlock_release_safe(&entry->lock, flags);

            return 0;
        }

        task_t* curr = proc_current();
        if (!curr) {
            spinlock_release_safe(&entry->lock, flags);

            return -1;
        }

        curr->blocked_on_sem = (void*)entry;

        dlist_add_tail(&curr->sem_node, &entry->wait_list);
        curr->state = TASK_WAITING;

        spinlock_release_safe(&entry->lock, flags);

        sched_yield();
    }
}

static int futex_do_wake(futex_entry_t* entry, uint32_t max_wake) {
    if (!entry || max_wake == 0u) {
        return 0;
    }

    const uint32_t flags = spinlock_acquire_safe(&entry->lock);

    uint32_t woken = 0;
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

    spinlock_release_safe(&entry->lock, flags);

    return (int)woken;
}

}

extern "C" int futex_wait(uint32_t key, volatile const uint32_t* uaddr, uint32_t expected) {
    futex_entry_t* entry = futex_acquire_entry(key, true);
    if (!entry) {
        return -1;
    }

    const int rc = futex_do_wait(entry, uaddr, expected);

    futex_release_entry(entry);

    return rc;
}

extern "C" int futex_wake(uint32_t key, uint32_t max_wake) {
    futex_entry_t* entry = futex_acquire_entry(key, false);
    if (!entry) {
        return 0;
    }

    const int rc = futex_do_wake(entry, max_wake);

    futex_release_entry(entry);

    return rc;
}
