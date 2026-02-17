// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#pragma once

#include <lib/types.h>
#include <hal/lock.h>
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
        clear();
    }

    void init() {
        for (size_t i = 0; i < Buckets; i++) {
            buckets[i].head = nullptr;
            spinlock_init(&buckets[i].lock);
        }
    }

    void insert(const K& key, const V& value) {
        (void)insert_or_assign(key, value);
    }

    bool insert_unique(const K& key, const V& value) {
        const uint32_t h = bucket_index(key);
        Bucket& b = buckets[h];

        uint32_t flags = spinlock_acquire_safe(&b.lock);

        Entry* e = b.head;
        while (e) {
            if (e->key == key) {
                spinlock_release_safe(&b.lock, flags);
                return false;
            }
            e = e->next;
        }

        Entry* new_entry = (Entry*)kmalloc(sizeof(Entry));
        if (!new_entry) {
            spinlock_release_safe(&b.lock, flags);
            return false;
        }

        new_entry->key = key;
        new_entry->value = value;
        new_entry->next = b.head;
        b.head = new_entry;

        spinlock_release_safe(&b.lock, flags);
        return true;
    }

    bool insert_or_assign(const K& key, const V& value) {
        const uint32_t h = bucket_index(key);
        Bucket& b = buckets[h];

        uint32_t flags = spinlock_acquire_safe(&b.lock);

        Entry* e = b.head;
        while (e) {
            if (e->key == key) {
                e->value = value;
                spinlock_release_safe(&b.lock, flags);
                return true;
            }
            e = e->next;
        }

        Entry* new_entry = (Entry*)kmalloc(sizeof(Entry));
        if (!new_entry) {
            spinlock_release_safe(&b.lock, flags);
            return false;
        }

        new_entry->key = key;
        new_entry->value = value;
        new_entry->next = b.head;
        b.head = new_entry;

        spinlock_release_safe(&b.lock, flags);
        return true;
    }

    bool find(const K& key, V& out_value) {
        const uint32_t h = bucket_index(key);
        Bucket& b = buckets[h];

        uint32_t flags = spinlock_acquire_safe(&b.lock);

        Entry* e = b.head;
        while (e) {
            if (e->key == key) {
                out_value = e->value;
                spinlock_release_safe(&b.lock, flags);
                return true;
            }
            e = e->next;
        }

        spinlock_release_safe(&b.lock, flags);
        return false;
    }

    bool remove(const K& key) {
        const uint32_t h = bucket_index(key);
        Bucket& b = buckets[h];

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
                kfree(e);
                spinlock_release_safe(&b.lock, flags);
                return true;
            }
            prev = e;
            e = e->next;
        }
        
        spinlock_release_safe(&b.lock, flags);
        return false;
    }

    template<typename F>
    bool with_value(const K& key, F func) {
        const uint32_t h = bucket_index(key);
        Bucket& b = buckets[h];

        uint32_t flags = spinlock_acquire_safe(&b.lock);

        Entry* e = b.head;
        while (e) {
            if (e->key == key) {
                bool ok = func(e->value);
                spinlock_release_safe(&b.lock, flags);
                return ok;
            }
            e = e->next;
        }

        spinlock_release_safe(&b.lock, flags);
        return false;
    }

    void clear() {
        for (size_t i = 0; i < Buckets; i++) {
            Bucket& b = buckets[i];
            uint32_t flags = spinlock_acquire_safe(&b.lock);
            Entry* e = b.head;
            while (e) {
                Entry* next = e->next;
                kfree(e);
                e = next;
            }
            b.head = nullptr;
            spinlock_release_safe(&b.lock, flags);
        }
    }

private:
    struct Bucket {
        spinlock_t lock;
        Entry* head;
    };

    Bucket buckets[Buckets];

    uint32_t bucket_index(const K& key) const {
        return hash(key) % Buckets;
    }

    static uint32_t hash(const K& key);
};
