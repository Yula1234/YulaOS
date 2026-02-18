// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 Yula1234

#pragma once

#include <stddef.h>

#include <lib/types.h>
#include <lib/cpp/atomic.h>
#include <lib/cpp/lock_guard.h>
#include <lib/cpp/new.h>
#include <mm/heap.h>
#include <lib/string.h>

template<typename K, typename V, size_t Buckets = 32>
class HashMap {
private:
    struct Entry {
        K key;
        V value;
        Entry* next;
    };

    struct Bucket {
        Entry* head;

        kernel::SpinLock lock;

        Bucket()
            : head(nullptr) {
        }
    };

    class Operation {
    public:
        explicit Operation(HashMap& map)
            : map_(&map) {
            ok_ = map_->begin_op(buckets_, mask_);
        }

        Operation(const Operation&) = delete;
        Operation& operator=(const Operation&) = delete;

        ~Operation() {
            if (ok_) {
                map_->end_op();
            }
        }

        explicit operator bool() const {
            return ok_;
        }

        Bucket* buckets() const {
            return buckets_;
        }

        size_t mask() const {
            return mask_;
        }

    private:
        HashMap* map_ = nullptr;
        Bucket* buckets_ = nullptr;
        size_t mask_ = 0u;
        bool ok_ = false;
    };

public:
    enum class InsertUniqueResult {
        Inserted,
        AlreadyPresent,
        OutOfMemory,
        Failed,
    };

    enum class InsertOrAssignResult {
        Inserted,
        Assigned,
        OutOfMemory,
        Failed,
    };

    HashMap() {
        init();
    }

    HashMap(const HashMap&) = delete;
    HashMap& operator=(const HashMap&) = delete;

    HashMap(HashMap&& other) noexcept {
        init();
        move_from(other);
    }

    HashMap& operator=(HashMap&& other) noexcept {
        if (this == &other) {
            return *this;
        }

        Bucket* old_buckets = nullptr;
        size_t old_bucket_count = 0u;

        if (this < &other) {
            kernel::SpinLockSafeGuard lock_this(table_lock);
            kernel::SpinLockSafeGuard lock_other(other.table_lock);


            quiesce_locked();
            other.quiesce_locked();


            old_buckets = buckets;
            old_bucket_count = bucket_count;


            steal_from_locked(other);
        } else {
            kernel::SpinLockSafeGuard lock_other(other.table_lock);
            kernel::SpinLockSafeGuard lock_this(table_lock);


            quiesce_locked();
            other.quiesce_locked();


            old_buckets = buckets;
            old_bucket_count = bucket_count;


            steal_from_locked(other);
        }

        if (old_buckets && old_bucket_count > 0u) {
            destroy_buckets(old_buckets, old_bucket_count);
        }

        return *this;
    }

    ~HashMap() {
        destroy();
    }

    class LockedValue {
    public:
        LockedValue() = default;

        LockedValue(HashMap& map, const K& key)
            : op_(map) {
            if (!op_) {
                return;
            }

            Bucket* local_buckets = op_.buckets();
            const size_t local_mask = op_.mask();


            const uint32_t h = map.bucket_index(key, local_mask);

            bucket_ = &local_buckets[h];


            lock_ = new (&lock_storage_) kernel::SpinLockSafeGuard(bucket_->lock);

            Entry* e = bucket_->head;


            while (e) {
                if (e->key == key) {
                    entry_ = e;
                    break;
                }

                e = e->next;
            }
        }

        LockedValue(const LockedValue&) = delete;
        LockedValue& operator=(const LockedValue&) = delete;

        LockedValue(LockedValue&&) = delete;
        LockedValue& operator=(LockedValue&&) = delete;

        ~LockedValue() {
            if (lock_) {
                lock_->~SpinLockSafeGuard();
            }
        }

        explicit operator bool() const {
            return entry_ != nullptr;
        }

        V* value_ptr() {
            if (!entry_) {
                return nullptr;
            }

            return &entry_->value;
        }

        const V* value_ptr() const {
            if (!entry_) {
                return nullptr;
            }

            return &entry_->value;
        }

    private:
        Operation op_;
        Bucket* bucket_ = nullptr;
        Entry* entry_ = nullptr;

        alignas(kernel::SpinLockSafeGuard) unsigned char lock_storage_[
            sizeof(kernel::SpinLockSafeGuard)
        ];
        kernel::SpinLockSafeGuard* lock_ = nullptr;
    };

    LockedValue find_ptr(const K& key) {
        return LockedValue(*this, key);
    }

    LockedValue find_ptr(const K& key) const {
        return LockedValue(*const_cast<HashMap*>(this), key);
    }


    bool contains(const K& key) {
        return (bool)find_ptr(key);
    }

    bool contains(const K& key) const {
        return (bool)find_ptr(key);
    }


    bool try_get(const K& key, V& out_value) {
        return find(key, out_value);
    }

    bool try_get(const K& key, V& out_value) const {
        return const_cast<HashMap*>(this)->find(key, out_value);
    }


    void insert(const K& key, const V& value) {
        (void)insert_or_assign(key, value);
    }

    bool insert_unique(const K& key, const V& value) {
        return insert_unique_ex(key, value) == InsertUniqueResult::Inserted;
    }

    InsertUniqueResult insert_unique_ex(const K& key, const V& value) {
        uint32_t new_size = 0u;

        {
            Operation op(*this);
            if (!op) {
                return InsertUniqueResult::Failed;
            }

            Bucket* local_buckets = op.buckets();
            const size_t local_mask = op.mask();

            const uint32_t h = bucket_index(key, local_mask);
            Bucket& b = local_buckets[h];

            kernel::SpinLockSafeGuard lock(b.lock);

            Entry* e = b.head;

            while (e) {
                if (e->key == key) {
                    return InsertUniqueResult::AlreadyPresent;
                }

                e = e->next;
            }

            Entry* new_entry = create_entry(key, value);
            if (!new_entry) {
                return InsertUniqueResult::OutOfMemory;
            }

            new_entry->next = b.head;
            b.head = new_entry;


            new_size = size_.fetch_add(1u, kernel::memory_order::seq_cst) + 1u;
        }

        maybe_resize(new_size);
        return InsertUniqueResult::Inserted;
    }

    bool insert_or_assign(const K& key, const V& value) {
        const auto result = insert_or_assign_ex(key, value);

        return result == InsertOrAssignResult::Inserted
            || result == InsertOrAssignResult::Assigned;
    }

    InsertOrAssignResult insert_or_assign_ex(const K& key, const V& value) {
        uint32_t new_size = 0u;
        bool should_resize = false;
        InsertOrAssignResult result = InsertOrAssignResult::Failed;

        {
            Operation op(*this);
            if (!op) {
                return InsertOrAssignResult::Failed;
            }

            Bucket* local_buckets = op.buckets();
            const size_t local_mask = op.mask();

            const uint32_t h = bucket_index(key, local_mask);
            Bucket& b = local_buckets[h];

            kernel::SpinLockSafeGuard lock(b.lock);

            Entry* e = b.head;

            while (e) {
                if (e->key == key) {
                    e->value = value;
                    result = InsertOrAssignResult::Assigned;
                    should_resize = false;
                    new_size = 0u;

                    break;
                }

                e = e->next;
            }

            if (result == InsertOrAssignResult::Assigned) {
                return result;
            }


            Entry* new_entry = create_entry(key, value);
            if (!new_entry) {
                return InsertOrAssignResult::OutOfMemory;
            }

            new_entry->next = b.head;
            b.head = new_entry;


            new_size = size_.fetch_add(1u, kernel::memory_order::seq_cst) + 1u;

            should_resize = true;
            result = InsertOrAssignResult::Inserted;
        }

        if (should_resize) {
            maybe_resize(new_size);
        }

        return result;
    }

    bool find(const K& key, V& out_value) {
        Operation op(*this);
        if (!op) {
            return false;
        }

        Bucket* local_buckets = op.buckets();
        const size_t local_mask = op.mask();

        const uint32_t h = bucket_index(key, local_mask);
        Bucket& b = local_buckets[h];

        kernel::SpinLockSafeGuard lock(b.lock);

        Entry* e = b.head;

        while (e) {
            if (e->key == key) {
                out_value = e->value;
                return true;
            }

            e = e->next;
        }

        return false;
    }

    bool remove(const K& key) {
        Operation op(*this);
        if (!op) {
            return false;
        }

        Bucket* local_buckets = op.buckets();
        const size_t local_mask = op.mask();

        const uint32_t h = bucket_index(key, local_mask);
        Bucket& b = local_buckets[h];

        kernel::SpinLockSafeGuard lock(b.lock);

        Entry* e = b.head;
        Entry* prev = nullptr;

        while (e) {
            if (e->key == key) {
                if (prev) {
                    prev->next = e->next;
                } else {
                    b.head = e->next;
                }

                destroy_entry(e);

                size_.fetch_sub(1u, kernel::memory_order::seq_cst);
                return true;
            }

            prev = e;
            e = e->next;
        }

        return false;
    }

    template<typename F>
    bool with_value_locked(const K& key, F func) {
        Operation op(*this);
        if (!op) {
            return false;
        }

        Bucket* local_buckets = op.buckets();
        const size_t local_mask = op.mask();

        const uint32_t h = bucket_index(key, local_mask);
        Bucket& b = local_buckets[h];


        kernel::SpinLockSafeGuard lock(b.lock);

        Entry* e = b.head;


        while (e) {
            if (e->key == key) {
                return func(e->value);
            }

            e = e->next;
        }

        return false;
    }

    template<typename F>
    bool with_value_unlocked(const K& key, F func) {
        alignas(V) unsigned char value_storage[sizeof(V)];
        bool has_value = false;

        {
            Operation op(*this);
            if (!op) {
                return false;
            }

            Bucket* local_buckets = op.buckets();
            const size_t local_mask = op.mask();

            const uint32_t h = bucket_index(key, local_mask);
            Bucket& b = local_buckets[h];

            kernel::SpinLockSafeGuard lock(b.lock);

            Entry* e = b.head;

            while (e) {
                if (e->key == key) {
                    new (value_storage) V(e->value);
                    has_value = true;
                    break;
                }

                e = e->next;
            }
        }

        if (!has_value) {
            return false;
        }

        V* value = (V*)value_storage;

        bool ok = func(*value);

        value->~V();
        return ok;
    }

    template<typename F>
    bool with_value(const K& key, F func) {
        return with_value_locked(key, func);
    }

    void clear() {
        kernel::SpinLockSafeGuard lock(table_lock);

        resizing.store(1u, kernel::memory_order::relaxed);
        kernel::spin_wait_equals(active_ops, 0u, kernel::memory_order::acquire);


        if (buckets && bucket_count > 0u) {
            for (size_t i = 0; i < bucket_count; i++) {
                clear_bucket(buckets[i]);
            }
        }


        size_.store(0u, kernel::memory_order::relaxed);
        resizing.store(0u, kernel::memory_order::relaxed);
    }

public:
    class LockedView {
    public:
        struct forward_iterator_tag {
        };

        struct pair_ref {
            K& first;
            V& second;
        };

        struct pair_const_ref {
            const K& first;
            const V& second;
        };


        class Iterator {
        public:
            using difference_type = ptrdiff_t;
            using value_type = pair_ref;
            using reference = pair_ref;
            using pointer = pair_ref*;
            using iterator_category = forward_iterator_tag;

            Iterator() = default;

            explicit Iterator(LockedView* view)
                : view_(view) {
                if (!view_ || !view_->map_ || !view_->map_->buckets) {
                    view_ = nullptr;
                    return;
                }

                bucket_index_ = 0u;
                entry_ = nullptr;
                advance_to_next();
            }

            Iterator(const Iterator&) = default;
            Iterator& operator=(const Iterator&) = default;

            bool operator==(const Iterator& other) const {
                return view_ == other.view_
                    && entry_ == other.entry_
                    && bucket_index_ == other.bucket_index_;
            }

            bool operator!=(const Iterator& other) const {
                return !(*this == other);
            }

            reference operator*() const {
                return {
                    entry_->key,
                    entry_->value,
                };
            }

            pointer operator->() const {
                return new (pair_storage_) pair_ref{
                    entry_->key,
                    entry_->value,
                };
            }

            Iterator& operator++() {
                if (!view_) {
                    return *this;
                }

                if (entry_) {
                    entry_ = entry_->next;
                }

                advance_to_next();
                return *this;
            }

            Iterator operator++(int) {
                Iterator before = *this;
                ++(*this);
                return before;
            }

        private:
            void advance_to_next() {
                while (view_ && !entry_) {
                    if (bucket_index_ >= view_->map_->bucket_count) {
                        view_ = nullptr;
                        return;
                    }

                    entry_ = view_->map_->buckets[bucket_index_].head;
                    if (!entry_) {
                        bucket_index_++;
                    }
                }
            }

            LockedView* view_ = nullptr;
            size_t bucket_index_ = 0u;
            Entry* entry_ = nullptr;

            alignas(pair_ref) mutable unsigned char pair_storage_[sizeof(pair_ref)];
        };


        class ConstIterator {
        public:
            using difference_type = ptrdiff_t;
            using value_type = pair_const_ref;
            using reference = pair_const_ref;
            using pointer = pair_const_ref*;
            using iterator_category = forward_iterator_tag;

            ConstIterator() = default;

            explicit ConstIterator(const LockedView* view)
                : view_(view) {
                if (!view_ || !view_->map_ || !view_->map_->buckets) {
                    view_ = nullptr;
                    return;
                }

                bucket_index_ = 0u;
                entry_ = nullptr;
                advance_to_next();
            }

            ConstIterator(const ConstIterator&) = default;
            ConstIterator& operator=(const ConstIterator&) = default;

            bool operator==(const ConstIterator& other) const {
                return view_ == other.view_
                    && entry_ == other.entry_
                    && bucket_index_ == other.bucket_index_;
            }

            bool operator!=(const ConstIterator& other) const {
                return !(*this == other);
            }

            reference operator*() const {
                return {
                    entry_->key,
                    entry_->value,
                };
            }

            pointer operator->() const {
                return new (pair_storage_) pair_const_ref{
                    entry_->key,
                    entry_->value,
                };
            }

            ConstIterator& operator++() {
                if (!view_) {
                    return *this;
                }

                if (entry_) {
                    entry_ = entry_->next;
                }

                advance_to_next();
                return *this;
            }

            ConstIterator operator++(int) {
                ConstIterator before = *this;
                ++(*this);
                return before;
            }

        private:
            void advance_to_next() {
                while (view_ && !entry_) {
                    if (bucket_index_ >= view_->map_->bucket_count) {
                        view_ = nullptr;
                        return;
                    }

                    entry_ = view_->map_->buckets[bucket_index_].head;
                    if (!entry_) {
                        bucket_index_++;
                    }
                }
            }

            const LockedView* view_ = nullptr;
            size_t bucket_index_ = 0u;
            Entry* entry_ = nullptr;

            alignas(pair_const_ref) mutable unsigned char pair_storage_[sizeof(pair_const_ref)];
        };

        using iterator = Iterator;
        using const_iterator = ConstIterator;


        explicit LockedView(HashMap& map)
            : map_(&map),
              lock_(map.table_lock) {
            map_->quiesce_locked();
        }

        LockedView(const LockedView&) = delete;
        LockedView& operator=(const LockedView&) = delete;

        ~LockedView() {
            map_->resizing.store(0u, kernel::memory_order::relaxed);
        }

        Iterator begin() {
            return Iterator(this);
        }

        Iterator end() {
            return Iterator();
        }


        ConstIterator begin() const {
            return ConstIterator(this);
        }

        ConstIterator end() const {
            return ConstIterator();
        }


        ConstIterator cbegin() const {
            return ConstIterator(this);
        }

        ConstIterator cend() const {
            return ConstIterator();
        }


    private:
        HashMap* map_ = nullptr;
        kernel::SpinLockSafeGuard lock_;
    };

    LockedView locked_view() {
        return LockedView(*this);
    }

private:
    static void destroy_buckets(Bucket* arr, size_t count) {
        if (!arr || count == 0u) {
            return;
        }

        for (size_t i = 0; i < count; i++) {
            clear_bucket(arr[i]);
            arr[i].~Bucket();
        }

        kfree(arr);
    }

    static Entry* create_entry(const K& key, const V& value) {
        Entry* e = new (kernel::nothrow) Entry;
        if (!e) {
            return nullptr;
        }

        e->key = key;
        e->value = value;
        e->next = nullptr;

        return e;
    }

    static void destroy_entry(Entry* entry) {
        delete entry;
    }

    static void clear_bucket(Bucket& bucket) {
        Entry* e = bucket.head;

        while (e) {
            Entry* next = e->next;
            destroy_entry(e);
            e = next;
        }

        bucket.head = nullptr;
    }

    void init() {
        active_ops.store(0u, kernel::memory_order::relaxed);
        resizing.store(0u, kernel::memory_order::relaxed);
        size_.store(0u, kernel::memory_order::relaxed);

        buckets = nullptr;
        bucket_count = 0u;
        bucket_mask = 0u;
    }

    void move_from(HashMap& other) noexcept {
        Bucket* old_buckets = nullptr;
        size_t old_bucket_count = 0u;

        {
            kernel::SpinLockSafeGuard lock_other(other.table_lock);

            other.quiesce_locked();

            old_buckets = buckets;
            old_bucket_count = bucket_count;

            steal_from_locked(other);
        }

        if (old_buckets && old_bucket_count > 0u) {
            destroy_buckets(old_buckets, old_bucket_count);
        }
    }

    void quiesce_locked() {
        resizing.store(1u, kernel::memory_order::relaxed);

        kernel::spin_wait_equals(active_ops, 0u, kernel::memory_order::acquire);
    }

    void steal_from_locked(HashMap& other) {
        buckets = other.buckets;
        bucket_count = other.bucket_count;
        bucket_mask = other.bucket_mask;

        size_.store(other.size_.load(kernel::memory_order::relaxed), kernel::memory_order::relaxed);

        other.buckets = nullptr;
        other.bucket_count = 0u;
        other.bucket_mask = 0u;

        other.size_.store(0u, kernel::memory_order::relaxed);
        other.resizing.store(0u, kernel::memory_order::relaxed);

        resizing.store(0u, kernel::memory_order::relaxed);
    }

    void destroy() {
        clear();

        if (buckets) {
            destroy_buckets(buckets, bucket_count);

            buckets = nullptr;
            bucket_count = 0u;
            bucket_mask = 0u;
        }
    }

    static constexpr uint32_t k_min_buckets = 8u;
    static constexpr uint32_t k_load_num = 3u;
    static constexpr uint32_t k_load_den = 4u;

    Bucket* buckets = nullptr;

    size_t bucket_count = 0u;
    size_t bucket_mask = 0u;

    kernel::SpinLock table_lock;

    kernel::atomic<uint32_t> size_{0u};
    kernel::atomic<uint32_t> resizing{0u};
    kernel::atomic<uint32_t> active_ops{0u};

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
            new (&arr[i]) Bucket();
        }

        return arr;
    }

    bool should_grow(uint32_t new_size) const {
        if (bucket_count == 0u) {
            return false;
        }

        return (uint64_t)new_size * k_load_den
            > (uint64_t)bucket_count * k_load_num;
    }

    size_t grow_target_count() const {
        return normalize_bucket_count(bucket_count * 2u);
    }

    Bucket* rehash_locked(Bucket* dst, size_t dst_count) {
        for (size_t i = 0; i < bucket_count; i++) {
            Entry* e = buckets[i].head;

            while (e) {
                Entry* next = e->next;

                const uint32_t h = bucket_index(e->key, dst_count - 1u);

                e->next = dst[h].head;
                dst[h].head = e;

                e = next;
            }

            buckets[i].head = nullptr;
        }

        Bucket* old = buckets;
        buckets = dst;
        bucket_count = dst_count;
        bucket_mask = dst_count - 1u;
        return old;
    }

    Bucket* try_resize_locked(uint32_t new_size, size_t& out_old_count) {
        if (resizing.load(kernel::memory_order::relaxed) != 0u) {
            return nullptr;
        }

        if (!should_grow(new_size)) {
            return nullptr;
        }

        quiesce_locked();

        out_old_count = bucket_count;

        const size_t target = grow_target_count();

        Bucket* new_buckets = allocate_buckets(target);
        if (!new_buckets) {
            resizing.store(0u, kernel::memory_order::relaxed);
            return nullptr;
        }


        Bucket* old_buckets = rehash_locked(new_buckets, target);


        resizing.store(0u, kernel::memory_order::relaxed);


        return old_buckets;
    }

    bool begin_op(Bucket*& out_buckets, size_t& out_mask) {
        while (1) {
            kernel::SpinLockSafeGuard lock(table_lock);

            if (resizing.load(kernel::memory_order::relaxed) != 0u) {
                kernel::cpu_relax();
                continue;
            }


            if (!ensure_buckets_locked()) {
                out_buckets = nullptr;
                out_mask = 0u;
                return false;
            }


            active_ops.fetch_add(1u, kernel::memory_order::seq_cst);

            out_buckets = buckets;
            out_mask = bucket_mask;

            return true;
        }
    }

    void end_op() {
        active_ops.fetch_sub(1u, kernel::memory_order::seq_cst);
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
        if (!should_grow(new_size)) {
            return;
        }

        Bucket* old_buckets = nullptr;
        size_t old_bucket_count = 0u;

        {
            kernel::SpinLockSafeGuard lock(table_lock);

            old_buckets = try_resize_locked(new_size, old_bucket_count);
        }

        if (old_buckets) {
            destroy_buckets(old_buckets, old_bucket_count);
        }
    }

    static uint32_t hash(const K& key);
};
