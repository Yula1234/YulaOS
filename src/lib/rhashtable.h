/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2026 Yula1234 */

#pragma once

#include <lib/cpp/hash_traits.h>
#include <lib/cpp/lock_guard.h>
#include <lib/cpp/utility.h>
#include <lib/cpp/atomic.h>
#include <lib/cpp/new.h>

#include <kernel/smp/cpu.h>
#include <kernel/panic.h>
#include <kernel/rcu.h>

#include <lib/compiler.h>
#include <lib/atomic.h>
#include <lib/types.h>

#include <mm/heap.h>

#include <stddef.h>
#include <stdint.h>

namespace kernel {

struct alignas(2) RHashNode {
    atomic_ptr_t next_;
    uint32_t     hash_;
};

namespace rhashtable_detail {

#if __SIZEOF_POINTER__ == 8
    static constexpr bool      k_use_tags   = true;
    static constexpr uintptr_t k_nulls_bit  = uintptr_t{ 1 };
    static constexpr int       k_tag_shift  = 48;
    static constexpr uintptr_t k_tag_mask   = static_cast<uintptr_t>(0xffffULL) << k_tag_shift;
    static constexpr uintptr_t k_ptr_mask   = ~(k_tag_mask | k_nulls_bit);
#else
    static constexpr bool      k_use_tags   = false;
    static constexpr uintptr_t k_nulls_bit  = uintptr_t{ 1 };
    static constexpr int       k_tag_shift  = 0;
    static constexpr uintptr_t k_tag_mask   = 0;
    static constexpr uintptr_t k_ptr_mask   = ~k_nulls_bit;
#endif

___inline uint16_t tag_from_hash(uint32_t hash) noexcept {
    if constexpr (k_use_tags) {
        return static_cast<uint16_t>(hash >> 16u);
    } else {
        return 0;
    }
}

___inline void* encode_ptr(RHashNode* node, uint32_t hash) noexcept {
    const uintptr_t raw = reinterpret_cast<uintptr_t>(node);
    
    if constexpr (k_use_tags) {
        const uintptr_t tag = static_cast<uintptr_t>(tag_from_hash(hash));
        return reinterpret_cast<void*>(raw | (tag << k_tag_shift));
    } else {
        return reinterpret_cast<void*>(raw);
    }
}

___inline RHashNode* decode_ptr(void* encoded) noexcept {
    return reinterpret_cast<RHashNode*>(
        reinterpret_cast<uintptr_t>(encoded) & k_ptr_mask
    );
}

___inline uint16_t tag_of(void* encoded) noexcept {
    if constexpr (k_use_tags) {
        return static_cast<uint16_t>(
            reinterpret_cast<uintptr_t>(encoded) >> k_tag_shift
        );
    } else {
        return 0;
    }
}
___inline bool is_nulls(void* ptr) noexcept {
    return (reinterpret_cast<uintptr_t>(ptr) & k_nulls_bit) != 0u;
}

___inline uint32_t nulls_bucket(void* ptr) noexcept {
    return static_cast<uint32_t>(
        (reinterpret_cast<uintptr_t>(ptr) & k_ptr_mask) >> 1u
    );
}

___inline void* make_nulls(uint32_t bucket) noexcept {
    return reinterpret_cast<void*>(
        (static_cast<uintptr_t>(bucket) << 1u) | k_nulls_bit
    );
}

template <typename V, typename M, M V::* Member>
___inline size_t member_offset() noexcept {
    constexpr uintptr_t k_fake_base = 0x1000u;

    return reinterpret_cast<size_t>(
        &(reinterpret_cast<V*>(k_fake_base)->*Member)
    ) - k_fake_base;
}

enum class WalkResult {
    Found,
    NotFound,
    Retry
};

}

template <
    typename K,
    typename V,
    RHashNode V::* NodeMember,
    typename KeyExtractor,
    typename Hash           = HashTraits<K>,
    size_t InitialBuckets   = 256
>
class RHashTable {

    struct Bucket {
        SpinLock     lock;
        atomic_ptr_t head_;
    };

    static_assert(sizeof(Bucket) <= 16u,
        "Bucket must fit in 16 bytes to keep four per cache line");

    struct BucketArray {
        rcu_head_t rcu;
        size_t     capacity;
        size_t     mask;
        Bucket     buckets[];
    };

    struct alignas(64) PerCpuCounter {
        atomic<int32_t> count{ 0 };
    };

    static constexpr size_t   k_resize_threshold  = 2u;
    static constexpr uint32_t k_check_interval    = 64u;
    static constexpr size_t   k_prefetch_distance = 8u;

    static V* value_from_node_(RHashNode* node) noexcept {
        if (kernel::unlikely(!node)) {
            return nullptr;
        }

        const size_t offset =
            rhashtable_detail::member_offset<V, RHashNode, NodeMember>();

        return reinterpret_cast<V*>(
            reinterpret_cast<char*>(node) - offset
        );
    }

    static RHashNode* node_from_value_(V* value) noexcept {
        if (kernel::unlikely(!value)) {
            return nullptr;
        }

        return &(value->*NodeMember);
    }

    static BucketArray* allocate_buckets_(size_t capacity) noexcept {
        const size_t byte_size =
            sizeof(BucketArray) + capacity * sizeof(Bucket);

        void* mem = kmalloc(byte_size);
        if (kernel::unlikely(!mem)) {
            return nullptr;
        }

        BucketArray* tbl = new (mem) BucketArray();

        tbl->capacity = capacity;
        tbl->mask     = capacity - 1u;

        for (size_t i = 0; i < capacity; ++i) {
            new (&tbl->buckets[i]) Bucket();

            atomic_ptr_set(
                &tbl->buckets[i].head_,
                rhashtable_detail::make_nulls(static_cast<uint32_t>(i))
            );
        }

        return tbl;
    }

    static void free_bucket_array_rcu_(rcu_head_t* head) noexcept {
        BucketArray* tbl = container_of(head, BucketArray, rcu);

        for (size_t i = 0; i < tbl->capacity; ++i) {
            tbl->buckets[i].~Bucket();
        }

        tbl->~BucketArray();
        kfree(tbl);
    }

    void add_count_(int32_t delta) noexcept {
        cpu_t*    cpu = cpu_current();
        const int idx = cpu ? cpu->index : 0;

        size_counters_[idx].count.fetch_add(delta, memory_order::relaxed);
    }

    uint32_t approx_count_() const noexcept {
        int32_t total = 0;

        for (int i = 0; i < MAX_CPUS; ++i) {
            total += size_counters_[i].count.load(memory_order::relaxed);
        }

        return total < 0 ? 0u : static_cast<uint32_t>(total);
    }

    bool should_check_resize_(BucketArray* tbl) noexcept {
        const uint32_t epoch =
            insert_epoch_.fetch_add(1u, memory_order::relaxed);

        if ((epoch & (k_check_interval - 1u)) != 0u) {
            return false;
        }

        return approx_count_() > tbl->capacity * k_resize_threshold;
    }

    rhashtable_detail::WalkResult walk_chain_rcu_(
        atomic_ptr_t* head_ptr,
        uint32_t      bucket_idx,
        uint32_t      hash,
        const K&      key,
        V**           out_val
    ) const noexcept {
        using namespace rhashtable_detail;

        KeyExtractor    extract;
        const uint16_t  want_tag = tag_from_hash(hash);

        void* ptr = atomic_ptr_read(head_ptr);

        while (!is_nulls(ptr)) {

            if (kernel::likely(tag_of(ptr) != want_tag)) {
                RHashNode* node = decode_ptr(ptr);
                ptr = atomic_ptr_read(&node->next_);
                continue;
            }

            RHashNode* node = decode_ptr(ptr);
            void*      next = atomic_ptr_read(&node->next_);

            __builtin_prefetch(decode_ptr(next), 0, 1);

            V* val = value_from_node_(node);

            if (kernel::likely(extract(*val) == key)) {
                *out_val = val;
                return WalkResult::Found;
            }

            ptr = next;
        }

        if (kernel::unlikely(nulls_bucket(ptr) != bucket_idx)) {
            return WalkResult::Retry;
        }

        return WalkResult::NotFound;
    }

    void rehash_bucket_(
        BucketArray* old_tbl,
        BucketArray* new_tbl,
        size_t       i
    ) noexcept {
        using namespace rhashtable_detail;

        const uint32_t low_nulls  = static_cast<uint32_t>(i);
        const uint32_t high_nulls = static_cast<uint32_t>(i + old_tbl->capacity);

        RHashNode* low_head  = static_cast<RHashNode*>(make_nulls(low_nulls));
        RHashNode* low_tail  = nullptr;

        RHashNode* high_head = static_cast<RHashNode*>(make_nulls(high_nulls));
        RHashNode* high_tail = nullptr;

        void* ptr = atomic_ptr_read(&old_tbl->buckets[i].head_);

        while (!is_nulls(ptr)) {
            RHashNode* node = decode_ptr(ptr);
            void*      next = atomic_ptr_read(&node->next_);

            if (!is_nulls(next)) {
                void* next_next =
                    atomic_ptr_read(&decode_ptr(next)->next_);
                __builtin_prefetch(decode_ptr(next_next), 0, 0);
            }

            if (node->hash_ & static_cast<uint32_t>(old_tbl->capacity)) {
                if (high_tail) {
                    atomic_ptr_set(
                        &high_tail->next_,
                        encode_ptr(node, node->hash_)
                    );
                } else {
                    high_head = node;
                }
                high_tail = node;
            } else {
                if (low_tail) {
                    atomic_ptr_set(
                        &low_tail->next_,
                        encode_ptr(node, node->hash_)
                    );
                } else {
                    low_head = node;
                }
                low_tail = node;
            }

            ptr = next;
        }

        if (low_tail) {
            atomic_ptr_set(&low_tail->next_, make_nulls(low_nulls));
        }

        if (high_tail) {
            atomic_ptr_set(&high_tail->next_, make_nulls(high_nulls));
        }

        void* low_encoded = (low_head == static_cast<RHashNode*>(make_nulls(low_nulls)))
            ? make_nulls(low_nulls)
            : encode_ptr(low_head, low_head->hash_);

        void* high_encoded = (high_head == static_cast<RHashNode*>(make_nulls(high_nulls)))
            ? make_nulls(high_nulls)
            : encode_ptr(high_head, high_head->hash_);

        atomic_ptr_store_explicit(
            &new_tbl->buckets[i].head_,
            low_encoded,
            ATOMIC_RELEASE
        );

        atomic_ptr_store_explicit(
            &new_tbl->buckets[i + old_tbl->capacity].head_,
            high_encoded,
            ATOMIC_RELEASE
        );
    }

    void schedule_resize_() noexcept {
        if (!resize_lock_.try_acquire()) {
            return;
        }

        BucketArray* old_tbl = current_table_.read();

        if (!old_tbl
                || approx_count_() < old_tbl->capacity * k_resize_threshold)
        {
            resize_lock_.release();
            return;
        }

        const size_t new_cap = old_tbl->capacity * 2u;

        BucketArray* new_tbl = allocate_buckets_(new_cap);
        if (kernel::unlikely(!new_tbl)) {
            resize_lock_.release();
            return;
        }

        const uint32_t irq_flags = irq_save();

        for (size_t i = 0; i < old_tbl->capacity; ++i) {
            old_tbl->buckets[i].lock.acquire();
        }

        for (size_t i = 0; i < old_tbl->capacity; ++i) {
            if (i + k_prefetch_distance < old_tbl->capacity) {
                __builtin_prefetch(
                    &old_tbl->buckets[i + k_prefetch_distance],
                    0, 0
                );
            }

            rehash_bucket_(old_tbl, new_tbl, i);
        }

        current_table_.assign(new_tbl);

        for (size_t i = 0; i < old_tbl->capacity; ++i) {
            old_tbl->buckets[i].lock.release();
        }

        irq_restore(irq_flags);

        call_rcu(&old_tbl->rcu, free_bucket_array_rcu_);

        resize_lock_.release();
    }

    V* find_unprotected_(const K& key) const noexcept {
        using namespace rhashtable_detail;

        const uint32_t hash = Hash::hash(key);

    retry:
        BucketArray* tbl = current_table_.read();
        if (kernel::unlikely(!tbl)) {
            return nullptr;
        }

        const uint32_t bucket_idx =
            hash & static_cast<uint32_t>(tbl->mask);

        V*         result      = nullptr;
        WalkResult walk_result = walk_chain_rcu_(
            &tbl->buckets[bucket_idx].head_,
            bucket_idx,
            hash,
            key,
            &result
        );

        if (kernel::unlikely(walk_result == WalkResult::Retry)) {
            goto retry;
        }

        return result;
    }

    RcuPtr<BucketArray> current_table_{};

    PerCpuCounter size_counters_[MAX_CPUS]{};

    atomic<uint32_t> insert_epoch_{ 0 };

    SpinLock resize_lock_{};

public:

    enum class InsertResult {
        Inserted,
        AlreadyPresent,
        Failed
    };

    RHashTable() {
        BucketArray* tbl = allocate_buckets_(InitialBuckets);
        if (kernel::likely(tbl)) {
            current_table_.assign(tbl);
        }
    }

    RHashTable(const RHashTable&)            = delete;
    RHashTable& operator=(const RHashTable&) = delete;

    RHashTable(RHashTable&&)                 = delete;
    RHashTable& operator=(RHashTable&&)      = delete;

    ~RHashTable() {
        BucketArray* tbl = current_table_.read();

        if (tbl) {
            for (size_t i = 0; i < tbl->capacity; ++i) {
                tbl->buckets[i].~Bucket();
            }

            tbl->~BucketArray();
            kfree(tbl);
        }
    }

    InsertResult insert_unique(V* value) noexcept {
        if (kernel::unlikely(!value)) {
            return InsertResult::Failed;
        }

        KeyExtractor   extract;
        const K&       key  = extract(*value);
        const uint32_t hash = Hash::hash(key);

        RHashNode* new_node = node_from_value_(value);
        new_node->hash_ = hash;

        RcuReadGuard r_guard;

    retry:
        BucketArray* tbl = current_table_.read();
        if (kernel::unlikely(!tbl)) {
            return InsertResult::Failed;
        }

        const uint32_t bucket_idx =
            hash & static_cast<uint32_t>(tbl->mask);

        Bucket& bucket = tbl->buckets[bucket_idx];

        {
            SpinLockSafeGuard s_guard(bucket.lock);

            if (kernel::unlikely(tbl != current_table_.read())) {
                goto retry;
            }

            const uint16_t want_tag =
                rhashtable_detail::tag_from_hash(hash);

            void* ptr = atomic_ptr_read(&bucket.head_);

            while (!rhashtable_detail::is_nulls(ptr)) {

                if (rhashtable_detail::tag_of(ptr) == want_tag) {
                    RHashNode* node = rhashtable_detail::decode_ptr(ptr);
                    V*         existing = value_from_node_(node);

                    if (extract(*existing) == key) {
                        return InsertResult::AlreadyPresent;
                    }
                }

                RHashNode* node = rhashtable_detail::decode_ptr(ptr);
                void*      next = atomic_ptr_read(&node->next_);

                __builtin_prefetch(rhashtable_detail::decode_ptr(next), 0, 1);

                ptr = next;
            }

            atomic_ptr_set(
                &new_node->next_,
                atomic_ptr_read(&bucket.head_)
            );

            atomic_ptr_store_explicit(
                &bucket.head_,
                rhashtable_detail::encode_ptr(new_node, hash),
                ATOMIC_RELEASE
            );
        }

        add_count_(1);

        if (kernel::unlikely(should_check_resize_(tbl))) {
            schedule_resize_();
        }

        return InsertResult::Inserted;
    }

    V* remove(const K& key) noexcept {
        const uint32_t hash = Hash::hash(key);
        KeyExtractor   extract;

        RcuReadGuard r_guard;

    retry:
        BucketArray* tbl = current_table_.read();
        if (kernel::unlikely(!tbl)) {
            return nullptr;
        }

        const uint32_t bucket_idx =
            hash & static_cast<uint32_t>(tbl->mask);

        Bucket& bucket = tbl->buckets[bucket_idx];

        {
            SpinLockSafeGuard s_guard(bucket.lock);

            if (kernel::unlikely(tbl != current_table_.read())) {
                goto retry;
            }

            const uint16_t want_tag =
                rhashtable_detail::tag_from_hash(hash);

            atomic_ptr_t* prev_ptr = &bucket.head_;
            void*         ptr      = atomic_ptr_read(prev_ptr);

            while (!rhashtable_detail::is_nulls(ptr)) {
                RHashNode* node = rhashtable_detail::decode_ptr(ptr);
                void*      next = atomic_ptr_read(&node->next_);

                __builtin_prefetch(rhashtable_detail::decode_ptr(next), 0, 1);

                if (rhashtable_detail::tag_of(ptr) == want_tag) {
                    V* val = value_from_node_(node);

                    if (extract(*val) == key) {
                        atomic_ptr_store_explicit(
                            prev_ptr, next, ATOMIC_RELEASE
                        );

                        add_count_(-1);

                        return val;
                    }
                }

                prev_ptr = &node->next_;
                ptr      = next;
            }
        }

        return nullptr;
    }

    template <typename F>
    bool with_value_unlocked(const K& key, F func) const noexcept {
        RcuReadGuard r_guard;

        V* val = find_unprotected_(key);
        if (!val) {
            return false;
        }

        return func(val);
    }
};

}