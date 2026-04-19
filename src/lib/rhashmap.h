/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2026 Yula1234 */

#pragma once

#include <lib/cpp/hash_traits.h>
#include <lib/cpp/lock_guard.h>
#include <lib/cpp/utility.h>
#include <lib/cpp/rwlock.h>
#include <lib/cpp/atomic.h>
#include <lib/cpp/new.h>

#include <kernel/smp/cpu.h>
#include <kernel/panic.h>
#include <kernel/rcu.h>

#include <lib/compiler.h>
#include <lib/types.h>

#include <mm/heap.h>

#include <stddef.h>
#include <stdint.h>

namespace kernel {

struct alignas(2) RHashNode {
    atomic_ptr_t next_;
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

        const size_t offset = reinterpret_cast<size_t>(&(reinterpret_cast<V*>(4096)->*NodeMember)) - 4096u;
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

public:
    enum class InsertResult {
        Inserted,
        AlreadyPresent,
        Failed
    };

    class RcuValueView {
    public:
        RcuValueView() = default;

        RcuValueView(const RHashTable& map, const K& key)
            : guard_() {
            value_ = map.find_unprotected_(key);
        }

        RcuValueView(const RcuValueView&) = delete;
        RcuValueView& operator=(const RcuValueView&) = delete;

        RcuValueView(RcuValueView&& other) noexcept
            : value_(other.value_) {
            other.value_ = nullptr;
        }

        RcuValueView& operator=(RcuValueView&& other) noexcept {
            if (this == &other) {
                return *this;
            }

            value_ = other.value_;
            other.value_ = nullptr;

            return *this;
        }

        ~RcuValueView() = default;

        explicit operator bool() const noexcept {
            return value_ != nullptr;
        }

        V* value_ptr() noexcept {
            return value_;
        }

        const V* value_ptr() const noexcept {
            return value_;
        }

        V* operator->() noexcept {
            return value_;
        }

        const V* operator->() const noexcept {
            return value_;
        }

    private:
        RcuReadGuard guard_;
        V* value_ = nullptr;
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

    RcuValueView find(const K& key) const noexcept {
        return RcuValueView(*this, key);
    }

    bool contains(const K& key) const noexcept {
        return static_cast<bool>(find(key));
    }

    InsertResult insert_unique(V* value) noexcept {
        if (!value) {
            return InsertResult::Failed;
        }

        KeyExtractor extract;
        const K& key = extract(*value);
        const uint32_t hash = Hash::hash(key);

        ReadGuard r_guard(resize_rwlock_);

        BucketArray* tbl = current_table_.read();
        if (!tbl) {
            return InsertResult::Failed;
        }

        const uint32_t bucket_idx = hash & tbl->mask;
        const uint32_t lock_idx = hash & k_lock_mask;

        {
            SpinLockSafeGuard s_guard(locks_[lock_idx]);

            void* ptr = atomic_ptr_read(&tbl->buckets[bucket_idx]);

            while (!is_nulls_(ptr)) {
                RHashNode* node = static_cast<RHashNode*>(ptr);
                V* val = value_from_node_(node);

                if (extract(*val) == key) {
                    return InsertResult::AlreadyPresent;
                }

                ptr = atomic_ptr_read(&node->next_);
            }

            RHashNode* new_node = node_from_value_(value);

            atomic_ptr_set(&new_node->next_, atomic_ptr_read(&tbl->buckets[bucket_idx]));
            atomic_ptr_store_explicit(&tbl->buckets[bucket_idx], new_node, ATOMIC_RELEASE);
        }

        const uint32_t current_size = size_.fetch_add(1u, memory_order::relaxed) + 1u;

        if (current_size > tbl->capacity * 2u) {
            maybe_resize_();
        }

        return InsertResult::Inserted;
    }

    V* remove(const K& key) noexcept {
        const uint32_t hash = Hash::hash(key);
        KeyExtractor extract;

        ReadGuard r_guard(resize_rwlock_);

        BucketArray* tbl = current_table_.read();
        if (!tbl) {
            return nullptr;
        }

        const uint32_t bucket_idx = hash & tbl->mask;
        const uint32_t lock_idx = hash & k_lock_mask;

        SpinLockSafeGuard s_guard(locks_[lock_idx]);

        atomic_ptr_t* prev_ptr = &tbl->buckets[bucket_idx];
        void* ptr = atomic_ptr_read(prev_ptr);

        while (!is_nulls_(ptr)) {
            RHashNode* node = static_cast<RHashNode*>(ptr);
            V* val = value_from_node_(node);

            if (extract(*val) == key) {
                void* next = atomic_ptr_read(&node->next_);
                atomic_ptr_store_explicit(prev_ptr, next, ATOMIC_RELEASE);
                
                size_.fetch_sub(1u, memory_order::relaxed);
                return val;
            }

            prev_ptr = &node->next_;
            ptr = atomic_ptr_read(prev_ptr);
        }

        return nullptr;
    }

    void clear() noexcept {
        WriteGuard w_guard(resize_rwlock_);
        
        BucketArray* tbl = current_table_.read();
        if (!tbl) {
            return;
        }

        for (size_t i = 0; i < tbl->capacity; ++i) {
            atomic_ptr_set(&tbl->buckets[i], make_nulls_(static_cast<uint32_t>(i)));
        }

        size_.store(0u, memory_order::relaxed);
    }

    template<typename F>
    bool with_value(const K& key, F func) const {
        RcuValueView view = find(key);

        if (!view) {
            return false;
        }

        func(*view.value_ptr());
        return true;
    }

private:
    friend class RcuValueView;

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
            V* val = value_from_node_(node);

            if (extract(*val) == key) {
                return val;
            }

            ptr = atomic_ptr_read(&node->next_);
        }

        if (get_nulls_bucket_(ptr) != bucket_idx) {
            goto retry;
        }

        return nullptr;
    }

    void maybe_resize_() noexcept {
        const uint32_t current_size = size_.load(memory_order::acquire);
        BucketArray* tbl = current_table_.read();

        if (!tbl || current_size < tbl->capacity * 2u) {
            return;
        }

        WriteGuard w_guard(resize_rwlock_);

        tbl = current_table_.read();
        if (current_size < tbl->capacity * 2u) {
            return;
        }

        const size_t new_cap = tbl->capacity * 2u;
        BucketArray* new_tbl = allocate_buckets_(new_cap);
        if (!new_tbl) {
            return;
        }

        KeyExtractor extract;

        for (size_t i = 0; i < tbl->capacity; ++i) {
            void* ptr = atomic_ptr_read(&tbl->buckets[i]);

            RHashNode* low_head = static_cast<RHashNode*>(make_nulls_(static_cast<uint32_t>(i)));
            RHashNode* low_tail = nullptr;

            RHashNode* high_head = static_cast<RHashNode*>(make_nulls_(static_cast<uint32_t>(i + tbl->capacity)));
            RHashNode* high_tail = nullptr;

            while (!is_nulls_(ptr)) {
                RHashNode* node = static_cast<RHashNode*>(ptr);
                V* val = value_from_node_(node);
                
                const uint32_t hash = Hash::hash(extract(*val));

                if (hash & tbl->capacity) {
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
                atomic_ptr_set(&high_tail->next_, make_nulls_(static_cast<uint32_t>(i + tbl->capacity)));
            }

            atomic_ptr_set(&new_tbl->buckets[i], low_head);
            atomic_ptr_set(&new_tbl->buckets[i + tbl->capacity], high_head);
        }

        current_table_.assign(new_tbl);

        call_rcu(&tbl->rcu, free_bucket_array_rcu_);
    }

    RcuPtr<BucketArray> current_table_{};
    
    atomic<uint32_t> size_{0u};
    
    RwLock resize_rwlock_{};
    SpinLock locks_[k_num_locks]{};
};

} // namespace kernel