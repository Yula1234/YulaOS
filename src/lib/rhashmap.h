/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2026 Yula1234 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include <lib/types.h>
#include <lib/compiler.h>
#include <lib/cpp/new.h>
#include <lib/cpp/atomic.h>
#include <lib/cpp/utility.h>
#include <lib/cpp/lock_guard.h>
#include <lib/cpp/hash_traits.h>

#include <kernel/rcu.h>
#include <kernel/smp/cpu.h>
#include <kernel/panic.h>

#include <mm/heap.h>

namespace kernel {

/*
 * RHashNode is embedded inside the value structure.
 * It is aligned to 2 bytes to reserve the lowest bit for the RCU 'nulls' marker.
 * Hash memoization avoids expensive re-hashing during table resize.
 */
struct alignas(2) RHashNode {
    atomic_ptr_t next_;
    uint32_t hash_;
};

template <
    typename K,
    typename V,
    RHashNode V::* NodeMember,
    typename KeyExtractor,
    typename Hash = HashTraits<K>,
    size_t InitialBuckets = 256
>
class RHashTable {
private:
    struct BucketArray {
        rcu_head_t rcu;

        size_t capacity;
        size_t mask;
        
        atomic_ptr_t buckets[];
    };

    struct alignas(64) BucketLock {
        SpinLock lock;
    };

    struct alignas(64) PerCpuCounter {
        atomic<int32_t> count{0};
    };

    static constexpr uintptr_t k_nulls_flag = 1u;
    static constexpr size_t k_num_locks = 64u;
    static constexpr size_t k_lock_mask = k_num_locks - 1u;

    static inline bool is_nulls_(void* ptr) noexcept {
        return (reinterpret_cast<uintptr_t>(ptr) & k_nulls_flag) != 0u;
    }

    static inline uint32_t get_nulls_bucket_(void* ptr) noexcept {
        return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(ptr) >> 1u);
    }

    static inline void* make_nulls_(uint32_t bucket) noexcept {
        return reinterpret_cast<void*>((static_cast<uintptr_t>(bucket) << 1u) | k_nulls_flag);
    }

    static inline V* value_from_node_(RHashNode* node) noexcept {
        if (!node) {
            return nullptr;
        }

        const size_t offset = reinterpret_cast<size_t>(&(reinterpret_cast<V*>(4096u)->*NodeMember)) - 4096u;

        return reinterpret_cast<V*>(reinterpret_cast<char*>(node) - offset);
    }

    static inline RHashNode* node_from_value_(V* value) noexcept {
        if (!value) {
            return nullptr;
        }

        return &(value->*NodeMember);
    }

    static BucketArray* allocate_buckets_(size_t capacity) noexcept {
        const size_t size = sizeof(BucketArray) + capacity * sizeof(atomic_ptr_t);
        
        void* mem = kmalloc(size);
        if (!mem) {
            return nullptr;
        }

        BucketArray* tbl = new (mem) BucketArray();

        tbl->capacity = capacity;
        tbl->mask = capacity - 1u;

        for (size_t i = 0; i < capacity; i++) {
            atomic_ptr_set(&tbl->buckets[i], make_nulls_(static_cast<uint32_t>(i)));
        }

        return tbl;
    }

    static void free_bucket_array_rcu_(rcu_head_t* head) noexcept {
        BucketArray* tbl = container_of(head, BucketArray, rcu);
        
        tbl->~BucketArray();
        kfree(tbl);
    }

    void add_size_(int32_t delta) noexcept {
        cpu_t* cpu = cpu_current();
        const int idx = cpu ? cpu->index : 0;

        size_counters_[idx].count.fetch_add(delta, memory_order::relaxed);
    }

    uint32_t get_size_() const noexcept {
        int32_t total = 0;

        for (int i = 0; i < MAX_CPUS; i++) {
            total += size_counters_[i].count.load(memory_order::relaxed);
        }

        return total < 0 ? 0u : static_cast<uint32_t>(total);
    }

public:
    enum class InsertResult {
        Inserted,
        AlreadyPresent,
        Failed
    };

    RHashTable() {
        BucketArray* tbl = allocate_buckets_(InitialBuckets);
        if (tbl) {
            current_table_.assign(tbl);
        }
    }

    RHashTable(const RHashTable&) = delete;
    RHashTable& operator=(const RHashTable&) = delete;

    RHashTable(RHashTable&&) = delete;
    RHashTable& operator=(RHashTable&&) = delete;

    ~RHashTable() {
        BucketArray* tbl = current_table_.read();
        
        if (tbl) {
            tbl->~BucketArray();
            kfree(tbl);
        }
    }

    InsertResult insert_unique(V* value) noexcept {
        if (!value) {
            return InsertResult::Failed;
        }

        KeyExtractor extract;
        const K& key = extract(*value);
        const uint32_t hash = Hash::hash(key);

        RHashNode* new_node = node_from_value_(value);
        new_node->hash_ = hash;

        RcuReadGuard r_guard;

    retry:
        BucketArray* tbl = current_table_.read();
        if (!tbl) {
            return InsertResult::Failed;
        }

        const uint32_t lock_idx = hash & k_lock_mask;

        {
            SpinLockSafeGuard s_guard(locks_[lock_idx].lock);

            if (tbl != current_table_.read()) {
                goto retry;
            }

            const uint32_t bucket_idx = hash & tbl->mask;
            void* ptr = atomic_ptr_read(&tbl->buckets[bucket_idx]);

            while (!is_nulls_(ptr)) {
                RHashNode* node = static_cast<RHashNode*>(ptr);
                
                if (node->hash_ == hash) {
                    V* val = value_from_node_(node);

                    if (extract(*val) == key) {
                        return InsertResult::AlreadyPresent;
                    }
                }

                ptr = atomic_ptr_read(&node->next_);
            }

            atomic_ptr_set(&new_node->next_, atomic_ptr_read(&tbl->buckets[bucket_idx]));
            atomic_ptr_store_explicit(&tbl->buckets[bucket_idx], new_node, ATOMIC_RELEASE);
        }

        add_size_(1);

        if (get_size_() > tbl->capacity * 2u) {
            schedule_resize_();
        }

        return InsertResult::Inserted;
    }

    V* remove(const K& key) noexcept {
        const uint32_t hash = Hash::hash(key);
        KeyExtractor extract;

        RcuReadGuard r_guard;

    retry:
        BucketArray* tbl = current_table_.read();
        if (!tbl) {
            return nullptr;
        }

        const uint32_t lock_idx = hash & k_lock_mask;

        SpinLockSafeGuard s_guard(locks_[lock_idx].lock);

        if (tbl != current_table_.read()) {
            goto retry;
        }

        const uint32_t bucket_idx = hash & tbl->mask;

        atomic_ptr_t* prev_ptr = &tbl->buckets[bucket_idx];
        void* ptr = atomic_ptr_read(prev_ptr);

        while (!is_nulls_(ptr)) {
            RHashNode* node = static_cast<RHashNode*>(ptr);
            
            if (node->hash_ == hash) {
                V* val = value_from_node_(node);

                if (extract(*val) == key) {
                    void* next = atomic_ptr_read(&node->next_);
                    atomic_ptr_store_explicit(prev_ptr, next, ATOMIC_RELEASE);
                    
                    add_size_(-1);

                    return val;
                }
            }

            prev_ptr = &node->next_;
            ptr = atomic_ptr_read(prev_ptr);
        }

        return nullptr;
    }

    template<typename F>
    bool with_value_unlocked(const K& key, F func) const noexcept {
        RcuReadGuard r_guard;

        V* val = find_unprotected_(key);
        if (!val) {
            return false;
        }

        return func(val);
    }

private:
    V* find_unprotected_(const K& key) const noexcept {
        const uint32_t hash = Hash::hash(key);
        KeyExtractor extract;

    retry:
        BucketArray* tbl = current_table_.read();
        if (!tbl) {
            return nullptr;
        }

        const uint32_t bucket_idx = hash & tbl->mask;
        void* ptr = atomic_ptr_read(&tbl->buckets[bucket_idx]);

        while (!is_nulls_(ptr)) {
            RHashNode* node = static_cast<RHashNode*>(ptr);
            
            if (node->hash_ == hash) {
                V* val = value_from_node_(node);

                if (extract(*val) == key) {
                    return val;
                }
            }

            ptr = atomic_ptr_read(&node->next_);
        }

        if (get_nulls_bucket_(ptr) != bucket_idx) {
            goto retry;
        }

        return nullptr;
    }

    void schedule_resize_() noexcept {
        if (!resize_lock_.try_acquire()) {
            return;
        }

        BucketArray* old_tbl = current_table_.read();
        if (!old_tbl || get_size_() < old_tbl->capacity * 2u) {
            resize_lock_.release();
            return;
        }

        const size_t new_cap = old_tbl->capacity * 2u;

        BucketArray* new_tbl = allocate_buckets_(new_cap);
        if (!new_tbl) {
            resize_lock_.release();
            return;
        }

        uint32_t irq_flags = irq_save();

        for (size_t i = 0; i < k_num_locks; i++) {
            locks_[i].lock.acquire();
        }

        for (size_t i = 0; i < old_tbl->capacity; ++i) {
            void* ptr = atomic_ptr_read(&old_tbl->buckets[i]);

            RHashNode* low_head = static_cast<RHashNode*>(make_nulls_(static_cast<uint32_t>(i)));
            RHashNode* low_tail = nullptr;

            RHashNode* high_head = static_cast<RHashNode*>(make_nulls_(static_cast<uint32_t>(i + old_tbl->capacity)));
            RHashNode* high_tail = nullptr;

            while (!is_nulls_(ptr)) {
                RHashNode* node = static_cast<RHashNode*>(ptr);
                
                if (node->hash_ & old_tbl->capacity) {
                    if (high_tail) {
                        atomic_ptr_set(&high_tail->next_, node);
                    } else {
                        high_head = node;
                    }

                    high_tail = node;
                } else {
                    if (low_tail) {
                        atomic_ptr_set(&low_tail->next_, node);
                    } else {
                        low_head = node;
                    }

                    low_tail = node;
                }

                ptr = atomic_ptr_read(&node->next_);
            }

            if (low_tail) {
                atomic_ptr_set(&low_tail->next_, make_nulls_(static_cast<uint32_t>(i)));
            }

            if (high_tail) {
                atomic_ptr_set(&high_tail->next_, make_nulls_(static_cast<uint32_t>(i + old_tbl->capacity)));
            }

            atomic_ptr_set(&new_tbl->buckets[i], low_head);
            atomic_ptr_set(&new_tbl->buckets[i + old_tbl->capacity], high_head);
        }

        current_table_.assign(new_tbl);

        for (size_t i = 0; i < k_num_locks; i++) {
            locks_[i].lock.release();
        }

        irq_restore(irq_flags);

        call_rcu(&old_tbl->rcu, free_bucket_array_rcu_);

        resize_lock_.release();
    }

    RcuPtr<BucketArray> current_table_{};
    
    PerCpuCounter size_counters_[MAX_CPUS]{};
    
    SpinLock resize_lock_{};
    BucketLock locks_[k_num_locks]{};
};

} // namespace kernel