// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#pragma once

#include <lib/types.h>
#include <hal/lock.h>
#include <lib/cpp/new.h>
#include <mm/heap.h>
#include <lib/string.h>

template<typename K, typename V, size_t Buckets = 32>
class HashMap {
public:
    struct Entry {
        K key;
        V value;
        Entry* next;
    };

    HashMap() {
        init();
    }

    ~HashMap() {
        destroy();
    }

    void init() {
        spinlock_init(&table_lock);
        active_ops = 0u;
        resizing = 0u;
        size_ = 0u;
        buckets = nullptr;
        bucket_count = 0u;
        bucket_mask = 0u;
    }

    void insert(const K& key, const V& value) {
        (void)insert_or_assign(key, value);
    }

    bool insert_unique(const K& key, const V& value) {
        Bucket* local_buckets = nullptr;
        size_t local_mask = 0u;
        if (!begin_op(local_buckets, local_mask)) {
            return false;
        }

        const uint32_t h = bucket_index(key, local_mask);
        Bucket& b = local_buckets[h];

        uint32_t flags = spinlock_acquire_safe(&b.lock);

        Entry* e = b.head;
        while (e) {
            if (e->key == key) {
                spinlock_release_safe(&b.lock, flags);
                end_op();
                return false;
            }
            e = e->next;
        }

        Entry* new_entry = new (kernel::nothrow) Entry;
        if (!new_entry) {
            spinlock_release_safe(&b.lock, flags);
            end_op();
            return false;
        }

        new_entry->key = key;
        new_entry->value = value;
        new_entry->next = b.head;
        b.head = new_entry;

        spinlock_release_safe(&b.lock, flags);

        const uint32_t new_size = __sync_fetch_and_add(&size_, 1u) + 1u;
        end_op();
        maybe_resize(new_size);
        return true;
    }

    bool insert_or_assign(const K& key, const V& value) {
        Bucket* local_buckets = nullptr;
        size_t local_mask = 0u;
        if (!begin_op(local_buckets, local_mask)) {
            return false;
        }

        const uint32_t h = bucket_index(key, local_mask);
        Bucket& b = local_buckets[h];

        uint32_t flags = spinlock_acquire_safe(&b.lock);

        Entry* e = b.head;
        while (e) {
            if (e->key == key) {
                e->value = value;
                spinlock_release_safe(&b.lock, flags);
                end_op();
                return true;
            }
            e = e->next;
        }

        Entry* new_entry = new (kernel::nothrow) Entry;
        if (!new_entry) {
            spinlock_release_safe(&b.lock, flags);
            end_op();
            return false;
        }

        new_entry->key = key;
        new_entry->value = value;
        new_entry->next = b.head;
        b.head = new_entry;

        spinlock_release_safe(&b.lock, flags);

        const uint32_t new_size = __sync_fetch_and_add(&size_, 1u) + 1u;
        end_op();
        maybe_resize(new_size);
        return true;
    }

    bool find(const K& key, V& out_value) {
        Bucket* local_buckets = nullptr;
        size_t local_mask = 0u;
        if (!begin_op(local_buckets, local_mask)) {
            return false;
        }

        const uint32_t h = bucket_index(key, local_mask);
        Bucket& b = local_buckets[h];

        uint32_t flags = spinlock_acquire_safe(&b.lock);

        Entry* e = b.head;
        while (e) {
            if (e->key == key) {
                out_value = e->value;
                spinlock_release_safe(&b.lock, flags);
                end_op();
                return true;
            }
            e = e->next;
        }

        spinlock_release_safe(&b.lock, flags);
        end_op();
        return false;
    }

    bool remove(const K& key) {
        Bucket* local_buckets = nullptr;
        size_t local_mask = 0u;
        if (!begin_op(local_buckets, local_mask)) {
            return false;
        }

        const uint32_t h = bucket_index(key, local_mask);
        Bucket& b = local_buckets[h];

        uint32_t flags = spinlock_acquire_safe(&b.lock);

        Entry* e = b.head;
        Entry* prev = nullptr;
        
        while (e) {
            if (e->key == key) {
                if (prev) {
                    prev->next = e->next;
                } else {
                    b.head = e->next;
                }
                delete e;
                spinlock_release_safe(&b.lock, flags);
                __sync_sub_and_fetch(&size_, 1u);
                end_op();
                return true;
            }
            prev = e;
            e = e->next;
        }
        
        spinlock_release_safe(&b.lock, flags);
        end_op();
        return false;
    }

    template<typename F>
    bool with_value(const K& key, F func) {
        Bucket* local_buckets = nullptr;
        size_t local_mask = 0u;
        if (!begin_op(local_buckets, local_mask)) {
            return false;
        }

        const uint32_t h = bucket_index(key, local_mask);
        Bucket& b = local_buckets[h];

        uint32_t flags = spinlock_acquire_safe(&b.lock);

        Entry* e = b.head;
        while (e) {
            if (e->key == key) {
                bool ok = func(e->value);
                spinlock_release_safe(&b.lock, flags);
                end_op();
                return ok;
            }
            e = e->next;
        }

        spinlock_release_safe(&b.lock, flags);
        end_op();
        return false;
    }

    void clear() {
        uint32_t flags = spinlock_acquire_safe(&table_lock);
        resizing = 1u;
        while (__atomic_load_n(&active_ops, __ATOMIC_ACQUIRE) != 0u) {
            __asm__ volatile("pause");
        }

        if (buckets && bucket_count > 0u) {
            for (size_t i = 0; i < bucket_count; i++) {
                Entry* e = buckets[i].head;
                while (e) {
                    Entry* next = e->next;
                    delete e;
                    e = next;
                }
                buckets[i].head = nullptr;
            }
        }

        size_ = 0u;
        resizing = 0u;
        spinlock_release_safe(&table_lock, flags);
    }

    void destroy() {
        clear();
        if (buckets) {
            kfree(buckets);
            buckets = nullptr;
            bucket_count = 0u;
            bucket_mask = 0u;
        }
    }

private:
    struct Bucket {
        spinlock_t lock;
        Entry* head;
    };

    static constexpr uint32_t k_min_buckets = 8u;
    static constexpr uint32_t k_load_num = 3u;
    static constexpr uint32_t k_load_den = 4u;

    Bucket* buckets = nullptr;
    size_t bucket_count = 0u;
    size_t bucket_mask = 0u;
    uint32_t size_ = 0u;
    spinlock_t table_lock;
    volatile uint32_t active_ops = 0u;
    volatile uint32_t resizing = 0u;

    uint32_t bucket_index(const K& key, size_t mask) const {
        return hash(key) & (uint32_t)mask;
    }

    static size_t normalize_bucket_count(size_t value) {
        size_t v = value;
        if (v < k_min_buckets) {
            v = k_min_buckets;
        }

        size_t pow2 = 1u;
        while (pow2 < v) {
            pow2 <<= 1u;
        }
        return pow2;
    }

    static Bucket* allocate_buckets(size_t count) {
        if (count == 0u) {
            return nullptr;
        }

        Bucket* arr = (Bucket*)kmalloc(sizeof(Bucket) * count);
        if (!arr) {
            return nullptr;
        }

        for (size_t i = 0; i < count; i++) {
            arr[i].head = nullptr;
            spinlock_init(&arr[i].lock);
        }

        return arr;
    }

    bool begin_op(Bucket*& out_buckets, size_t& out_mask) {
        while (1) {
            uint32_t flags = spinlock_acquire_safe(&table_lock);
            if (resizing != 0u) {
                spinlock_release_safe(&table_lock, flags);
                __asm__ volatile("pause");
                continue;
            }

            if (!ensure_buckets_locked()) {
                out_buckets = nullptr;
                out_mask = 0u;
                spinlock_release_safe(&table_lock, flags);
                return false;
            }

            __sync_fetch_and_add(&active_ops, 1u);
            out_buckets = buckets;
            out_mask = bucket_mask;
            spinlock_release_safe(&table_lock, flags);
            return true;
        }
    }

    void end_op() {
        __sync_sub_and_fetch(&active_ops, 1u);
    }

    bool ensure_buckets_locked() {
        if (buckets && bucket_count > 0u) {
            return true;
        }

        const size_t init_count = normalize_bucket_count(Buckets);
        Bucket* fresh = allocate_buckets(init_count);
        if (!fresh) {
            return false;
        }

        buckets = fresh;
        bucket_count = init_count;
        bucket_mask = init_count - 1u;
        return true;
    }

    void maybe_resize(uint32_t new_size) {
        if (bucket_count == 0u) {
            return;
        }

        if ((uint64_t)new_size * k_load_den <= (uint64_t)bucket_count * k_load_num) {
            return;
        }

        uint32_t flags = spinlock_acquire_safe(&table_lock);
        if (resizing != 0u) {
            spinlock_release_safe(&table_lock, flags);
            return;
        }

        if ((uint64_t)new_size * k_load_den <= (uint64_t)bucket_count * k_load_num) {
            spinlock_release_safe(&table_lock, flags);
            return;
        }

        resizing = 1u;
        while (__atomic_load_n(&active_ops, __ATOMIC_ACQUIRE) != 0u) {
            __asm__ volatile("pause");
        }

        const size_t target = normalize_bucket_count(bucket_count * 2u);
        Bucket* new_buckets = allocate_buckets(target);
        if (!new_buckets) {
            resizing = 0u;
            spinlock_release_safe(&table_lock, flags);
            return;
        }

        for (size_t i = 0; i < bucket_count; i++) {
            Entry* e = buckets[i].head;
            while (e) {
                Entry* next = e->next;
                const uint32_t h = bucket_index(e->key, target - 1u);
                e->next = new_buckets[h].head;
                new_buckets[h].head = e;
                e = next;
            }
            buckets[i].head = nullptr;
        }

        Bucket* old_buckets = buckets;
        buckets = new_buckets;
        bucket_count = target;
        bucket_mask = target - 1u;

        resizing = 0u;
        spinlock_release_safe(&table_lock, flags);

        if (old_buckets) {
            kfree(old_buckets);
        }
    }

    static uint32_t hash(const K& key);
};
