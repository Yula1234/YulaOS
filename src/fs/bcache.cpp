/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2025 Yula1234 */

#include <lib/cpp/hash_traits.h>
#include <lib/cpp/rwlock.h>
#include <lib/cpp/atomic.h>
#include <lib/cpp/lock_guard.h>
#include <lib/cpp/new.h>
#include <lib/cpp/semaphore.h>

#include <lib/hash_map.h>
#include <lib/string.h>

#include <kernel/sched.h>
#include <kernel/cpu.h>

#include <mm/heap.h>
#include <mm/pmm.h>

#include <drivers/ahci.h>
#include <hal/lock.h>

#include "bcache.h"

static constexpr uint32_t BLOCK_SIZE = 4096;
static constexpr uint32_t SECTOR_SIZE = 512;
static constexpr uint32_t SECTORS_PER_BLK = 8;

static constexpr uint32_t BCACHE_SHARDS = 64;
static constexpr uint32_t BCACHE_SHARD_BUCKETS = 256;

static constexpr uint32_t HOT_SLOTS = 64;

static constexpr uint32_t k_flag_valid = 1u << 0;
static constexpr uint32_t k_flag_dirty = 1u << 1;
static constexpr uint32_t k_flag_io_inflight = 1u << 2;
static constexpr uint32_t k_flag_evicting = 1u << 3;
static constexpr uint32_t k_flag_accessed = 1u << 4;

static kmem_cache_t* g_entry_cache = nullptr;

struct BcacheIoEvent {
    kernel::atomic<int> done{0};

    kernel::Semaphore sem{};

    void init() {
        reset();
    }

    void reset() {
        done.store(0, kernel::memory_order::release);

        sem.init(0);
    }

    void wait() {
        while (done.load(kernel::memory_order::acquire) == 0) {
            sem.wait();
        }
    }

    void signal_all() {
        done.store(1, kernel::memory_order::release);

        sem.signal_all();
    }
};

struct BcacheEntry {
    uint32_t block_idx = 0;

    kernel::atomic<uint32_t> refcnt{0};
    kernel::atomic<uint32_t> flags{0};

    kernel::RwLock data_lock{};
    BcacheIoEvent io_done{};

    uint8_t* data = nullptr;

    BcacheEntry* clock_next = nullptr;
    BcacheEntry* clock_prev = nullptr;

    BcacheEntry() {
        io_done.init();
    }

    void get() {
        refcnt.fetch_add(1, kernel::memory_order::acquire);
    }

    void put() {
        const uint32_t prev = refcnt.fetch_sub(1, kernel::memory_order::release);
        if (prev != 1u) {
            return;
        }

        kernel::atomic_thread_fence(kernel::memory_order::acquire);

        const uint32_t f = flags.load(kernel::memory_order::acquire);
        if ((f & k_flag_evicting) == 0u) {
            return;
        }

        free_self();
    }

    ~BcacheEntry() = default;

private:
    void free_self() {
        if (data) {
            kfree(data);
            data = nullptr;
        }

        this->~BcacheEntry();

        if (g_entry_cache) {
            kmem_cache_free(g_entry_cache, this);
        } else {
            kfree(this);
        }
    }
};

struct HotSlot {
    uint32_t block_idx = 0;
    BcacheEntry* entry = nullptr;
};

struct PerCpuHotCache {
    kernel::SpinLock lock{};
    HotSlot slots[HOT_SLOTS]{};

    void init() {
        kernel::SpinLockSafeGuard guard(lock);

        for (uint32_t i = 0; i < HOT_SLOTS; i++) {
            slots[i] = {};
        }
    }

    BcacheEntry* try_get(uint32_t block_idx) {
        const uint32_t slot_idx = slot_index_for(block_idx);

        kernel::SpinLockSafeGuard guard(lock);

        HotSlot& slot = slots[slot_idx];
        if (!slot.entry || slot.block_idx != block_idx) {
            return nullptr;
        }

        slot.entry->get();

        return slot.entry;
    }

    void put(uint32_t block_idx, BcacheEntry& entry) {
        const uint32_t slot_idx = slot_index_for(block_idx);

        entry.get();

        BcacheEntry* old = nullptr;
        {
            kernel::SpinLockSafeGuard guard(lock);

            HotSlot& slot = slots[slot_idx];

            old = slot.entry;

            slot.block_idx = block_idx;
            slot.entry = &entry;
        }

        if (old) {
            old->put();
        }
    }

    uint32_t invalidate(BcacheEntry& entry) {
        uint32_t removed = 0;

        kernel::SpinLockSafeGuard guard(lock);

        for (uint32_t i = 0; i < HOT_SLOTS; i++) {
            HotSlot& slot = slots[i];
            if (slot.entry != &entry) {
                continue;
            }

            slot.block_idx = 0;
            slot.entry = nullptr;

            removed++;
        }

        return removed;
    }

private:
    static uint32_t slot_index_for(uint32_t block_idx) {
        return kernel::HashTraits<uint32_t>::hash(block_idx) & (HOT_SLOTS - 1u);
    }
};

struct HotCache {
    PerCpuHotCache per_cpu[MAX_CPUS]{};

    void init() {
        for (int cpu = 0; cpu < MAX_CPUS; cpu++) {
            per_cpu[cpu].init();
        }
    }

    BcacheEntry* try_get(uint32_t block_idx) {
        const int cpu_idx = cpu_index();
        if (cpu_idx < 0) {
            return nullptr;
        }

        return per_cpu[cpu_idx].try_get(block_idx);
    }

    void put(uint32_t block_idx, BcacheEntry& entry) {
        const uint32_t entry_flags = entry.flags.load(kernel::memory_order::acquire);
        if ((entry_flags & k_flag_evicting) != 0u) {
            return;
        }

        const int cpu_idx = cpu_index();
        if (cpu_idx < 0) {
            return;
        }

        per_cpu[cpu_idx].put(block_idx, entry);
    }

    void invalidate_entry(BcacheEntry& entry) {
        for (int cpu = 0; cpu < MAX_CPUS; cpu++) {
            const uint32_t removed = per_cpu[cpu].invalidate(entry);

            for (uint32_t i = 0; i < removed; i++) {
                entry.put();
            }
        }
    }

private:
    static int cpu_index() {
        cpu_t* cpu = cpu_current();
        const int cpu_idx = cpu ? cpu->index : 0;

        if (cpu_idx < 0 || cpu_idx >= MAX_CPUS) {
            return -1;
        }

        return cpu_idx;
    }
};

static HotCache g_hot_cache{};

static void disk_read_4k(uint32_t block_idx, uint8_t* buf);
static void disk_write_4k(uint32_t block_idx, const uint8_t* buf);

struct EvictedBlock {
    uint32_t block_idx = 0;

    BcacheEntry* entry = nullptr;

    bool dirty = false;
    uint8_t data[BLOCK_SIZE]{};
};

struct BcacheShard {
    kernel::RwLock meta_lock{};
    HashMap<uint32_t, BcacheEntry*, BCACHE_SHARD_BUCKETS> map{};

    BcacheEntry* clock_hand = nullptr;
    uint32_t entries = 0;
    uint32_t max_entries = 0;

    BcacheEntry* lookup_locked(uint32_t block_idx) {
        BcacheEntry* e = nullptr;
        if (!map.try_get(block_idx, e)) {
            return nullptr;
        }

        return e;
    }

    void clock_insert_locked(BcacheEntry& e) {
        if (!clock_hand) {
            clock_hand = &e;
            e.clock_next = &e;
            e.clock_prev = &e;
            return;
        }

        BcacheEntry* hand = clock_hand;
        BcacheEntry* prev = hand->clock_prev;

        e.clock_next = hand;
        e.clock_prev = prev;

        prev->clock_next = &e;
        hand->clock_prev = &e;
    }

    void clock_remove_locked(BcacheEntry& e) {
        if (!clock_hand) {
            return;
        }

        if (e.clock_next == &e) {
            clock_hand = nullptr;
            e.clock_next = nullptr;
            e.clock_prev = nullptr;
            return;
        }

        if (clock_hand == &e) {
            clock_hand = e.clock_next;
        }

        BcacheEntry* next = e.clock_next;
        BcacheEntry* prev = e.clock_prev;

        prev->clock_next = next;
        next->clock_prev = prev;

        e.clock_next = nullptr;
        e.clock_prev = nullptr;
    }

    bool try_evict_one_locked(EvictedBlock& out) {
        if (!clock_hand) {
            return false;
        }

        for (uint32_t i = 0; i < entries + 1u; i++) {
            BcacheEntry* cand = clock_hand;
            clock_hand = clock_hand->clock_next;

            const uint32_t refs = cand->refcnt.load(kernel::memory_order::acquire);
            if (refs != 1u) {
                continue;
            }

            uint32_t flags = cand->flags.load(kernel::memory_order::acquire);
            if ((flags & k_flag_accessed) != 0u) {
                cand->flags.fetch_and(
                    ~k_flag_accessed,
                    kernel::memory_order::acq_rel
                );
                continue;
            }

            cand->flags.fetch_or(
                k_flag_evicting,
                kernel::memory_order::acq_rel
            );

            map.remove(cand->block_idx);
            clock_remove_locked(*cand);
            entries--;

            out.block_idx = cand->block_idx;
            out.entry = cand;

            flags = cand->flags.load(kernel::memory_order::acquire);
            if ((flags & k_flag_dirty) != 0u) {
                kernel::ReadGuard data_guard(cand->data_lock);

                if (cand->data) {
                    memcpy(out.data, cand->data, BLOCK_SIZE);
                }

                out.dirty = true;

                cand->flags.fetch_and(
                    ~k_flag_dirty,
                    kernel::memory_order::acq_rel
                );
            }

            return true;
        }

        return false;
    }

    bool ensure_space() {
        while (entries >= max_entries && max_entries != 0u) {
            EvictedBlock ev{};

            {
                kernel::WriteGuard meta_guard(meta_lock);

                if (entries < max_entries) {
                    break;
                }

                if (!try_evict_one_locked(ev)) {
                    return false;
                }
            }

            if (ev.entry) {
                g_hot_cache.invalidate_entry(*ev.entry);
            }

            if (ev.dirty) {
                disk_write_4k(ev.block_idx, ev.data);
            }

            if (ev.entry) {
                ev.entry->put();
            }
        }

        return true;
    }

    BcacheEntry* get_or_create(
        uint32_t block_idx,
        bool for_write,
        const uint8_t* write_buf
    ) {
        {
            kernel::ReadGuard meta_guard(meta_lock);

            BcacheEntry* e = lookup_locked(block_idx);
            if (e) {
                e->get();
                return e;
            }
        }

        if (!ensure_space()) {
            return nullptr;
        }

        BcacheEntry* created = nullptr;

        {
            kernel::WriteGuard meta_guard(meta_lock);

            BcacheEntry* race = lookup_locked(block_idx);
            if (race) {
                race->get();
                return race;
            }

            void* mem = nullptr;
            if (g_entry_cache) {
                mem = kmem_cache_alloc(g_entry_cache);
            } else {
                mem = kmalloc(sizeof(BcacheEntry));
            }

            if (!mem) {
                return nullptr;
            }

            created = new (mem) BcacheEntry();
            created->block_idx = block_idx;

            created->refcnt.store(2u, kernel::memory_order::release);

            const uint32_t base_flags = for_write
                ? (k_flag_valid | k_flag_dirty | k_flag_accessed)
                : k_flag_io_inflight;
            created->flags.store(base_flags, kernel::memory_order::release);

            created->data = static_cast<uint8_t*>(kmalloc_a(BLOCK_SIZE));
            if (!created->data) {
                created->flags.fetch_or(
                    k_flag_evicting,
                    kernel::memory_order::acq_rel
                );

                created->refcnt.store(1u, kernel::memory_order::release);

                created->put();

                return nullptr;
            }

            if (for_write) {
                kernel::WriteGuard data_guard(created->data_lock);

                memcpy(created->data, write_buf, BLOCK_SIZE);
            } else {
                created->io_done.reset();
            }

            map.insert_or_assign(block_idx, created);
            clock_insert_locked(*created);

            entries++;
        }

        if (for_write) {
            return created;
        }

        uint8_t tmp[BLOCK_SIZE];

        disk_read_4k(block_idx, tmp);

        {
            kernel::WriteGuard data_guard(created->data_lock);

            memcpy(created->data, tmp, BLOCK_SIZE);
        }

        created->flags.fetch_or(
            k_flag_valid | k_flag_accessed,
            kernel::memory_order::acq_rel
        );

        created->flags.fetch_and(
            ~k_flag_io_inflight,
            kernel::memory_order::acq_rel
        );

        created->io_done.signal_all();

        return created;
    }
};

static BcacheShard g_shards[BCACHE_SHARDS]{};

static void disk_read_4k(uint32_t block_idx, uint8_t* buf) {
    if (!buf) {
        return;
    }

    const uint64_t start_lba = (uint64_t)block_idx * (uint64_t)SECTORS_PER_BLK;
    if (start_lba > 0xFFFFFFFF) {
        return;
    }

    ahci_read_sectors(
        (uint32_t)start_lba,
        SECTORS_PER_BLK,
        buf
    );
}

static void disk_write_4k(uint32_t block_idx, const uint8_t* buf) {
    if (!buf) {
        return;
    }

    const uint64_t start_lba = (uint64_t)block_idx * (uint64_t)SECTORS_PER_BLK;
    if (start_lba > 0xFFFFFFFF) {
        return;
    }

    ahci_write_sectors(
        (uint32_t)start_lba,
        SECTORS_PER_BLK,
        buf
    );
}

static uint32_t shard_index_for(uint32_t block_idx) {
    return kernel::HashTraits<uint32_t>::hash(block_idx) & (BCACHE_SHARDS - 1u);
}

void bcache_init(void) {
    if (!g_entry_cache) {
        g_entry_cache = kmem_cache_create(
            "bcache_e",
            sizeof(BcacheEntry),
            0,
            0
        );
    }

    const uint64_t total_ram_bytes =
        (uint64_t)pmm_get_total_blocks() * (uint64_t)BLOCK_SIZE;
    const uint64_t target_bytes = total_ram_bytes / 50u;

    uint64_t target_entries = target_bytes / (uint64_t)BLOCK_SIZE;

    if (target_entries < (uint64_t)BCACHE_SHARDS) {
        target_entries = BCACHE_SHARDS;
    }

    target_entries -= target_entries % (uint64_t)BCACHE_SHARDS;

    const uint32_t per_shard =
        (uint32_t)(target_entries / (uint64_t)BCACHE_SHARDS);

    for (uint32_t i = 0; i < BCACHE_SHARDS; i++) {
        kernel::WriteGuard meta_guard(g_shards[i].meta_lock);

        g_shards[i].map.clear();
        g_shards[i].clock_hand = nullptr;
        g_shards[i].entries = 0;
        g_shards[i].max_entries = per_shard;
    }

    g_hot_cache.init();
}

int bcache_read(uint32_t block_idx, uint8_t* buf) {
    if (!buf) {
        return 0;
    }

    if (BcacheEntry* hot = g_hot_cache.try_get(block_idx)) {
        {
            kernel::ReadGuard data_guard(hot->data_lock);

            memcpy(buf, hot->data, BLOCK_SIZE);
        }

        hot->flags.fetch_or(k_flag_accessed, kernel::memory_order::acq_rel);

        hot->put();
        return 1;
    }

    BcacheShard& shard = g_shards[shard_index_for(block_idx)];

    BcacheEntry* e = shard.get_or_create(block_idx, false, nullptr);
    if (!e) {
        return 0;
    }

    const uint32_t flags = e->flags.load(kernel::memory_order::acquire);
    if ((flags & k_flag_io_inflight) != 0u) {
        e->io_done.wait();
    }

    {
        kernel::ReadGuard data_guard(e->data_lock);

        memcpy(buf, e->data, BLOCK_SIZE);
    }

    e->flags.fetch_or(k_flag_accessed, kernel::memory_order::acq_rel);

    g_hot_cache.put(block_idx, *e);

    e->put();

    return 1;
}

int bcache_write(uint32_t block_idx, const uint8_t* buf) {
    if (!buf) {
        return 0;
    }

    if (BcacheEntry* hot = g_hot_cache.try_get(block_idx)) {
        {
            kernel::WriteGuard data_guard(hot->data_lock);

            memcpy(hot->data, buf, BLOCK_SIZE);
        }

        hot->flags.fetch_or(
            k_flag_dirty | k_flag_valid | k_flag_accessed,
            kernel::memory_order::acq_rel
        );

        hot->put();
        return 1;
    }

    BcacheShard& shard = g_shards[shard_index_for(block_idx)];

    BcacheEntry* e = shard.get_or_create(block_idx, true, buf);
    if (!e) {
        return 0;
    }

    const uint32_t flags = e->flags.load(kernel::memory_order::acquire);
    if ((flags & k_flag_io_inflight) != 0u) {
        e->io_done.wait();
    }

    {
        kernel::WriteGuard data_guard(e->data_lock);

        memcpy(e->data, buf, BLOCK_SIZE);
    }

    e->flags.fetch_or(
        k_flag_dirty | k_flag_valid | k_flag_accessed,
        kernel::memory_order::acq_rel
    );

    g_hot_cache.put(block_idx, *e);

    e->put();

    return 1;
}

void bcache_sync(void) {
    for (uint32_t si = 0; si < BCACHE_SHARDS; si++) {
        BcacheShard& shard = g_shards[si];

        while (true) {
            uint32_t blk = 0;
            uint8_t tmp[BLOCK_SIZE];

            BcacheEntry* e = nullptr;
            
            bool do_flush = false;

            {
                kernel::WriteGuard meta_guard(shard.meta_lock);

                if (!shard.clock_hand) {
                    break;
                }

                BcacheEntry* cur = shard.clock_hand;
                bool found = false;

                for (uint32_t i = 0; i < shard.entries + 1u; i++) {
                    const uint32_t flags =
                        cur->flags.load(kernel::memory_order::acquire);
                    const uint32_t refs =
                        cur->refcnt.load(kernel::memory_order::acquire);

                    if (refs >= 1u
                        && (flags & k_flag_dirty) != 0u
                        && (flags & k_flag_evicting) == 0u) {
                        cur->get();

                        cur->flags.fetch_and(
                            ~k_flag_dirty,
                            kernel::memory_order::acq_rel
                        );

                        {
                            kernel::ReadGuard data_guard(cur->data_lock);

                            memcpy(tmp, cur->data, BLOCK_SIZE);
                        }

                        blk = cur->block_idx;
                        e = cur;
                        do_flush = true;
                        found = true;
                        break;
                    }

                    cur = cur->clock_next;
                }

                if (!found) {
                    break;
                }
            }


            if (do_flush) {
                disk_write_4k(blk, tmp);
            }

            if (e) {
                e->put();
            }
        }
    }
}

void bcache_flush_block(uint32_t block_idx) {
    BcacheShard& shard = g_shards[shard_index_for(block_idx)];

    uint8_t tmp[BLOCK_SIZE];
    bool do_flush = false;

    {
        kernel::ReadGuard meta_guard(shard.meta_lock);

        BcacheEntry* e = shard.lookup_locked(block_idx);
        if (!e) {
            return;
        }

        e->get();

        const uint32_t flags = e->flags.load(kernel::memory_order::acquire);
        if ((flags & k_flag_dirty) == 0u) {
            e->put();
            return;
        }

        e->flags.fetch_and(
            ~k_flag_dirty,
            kernel::memory_order::acq_rel
        );

        {
            kernel::ReadGuard data_guard(e->data_lock);

            memcpy(tmp, e->data, BLOCK_SIZE);
        }

        do_flush = true;

        e->put();
    }

    if (do_flush) {
        disk_write_4k(block_idx, tmp);
    }
}

void bcache_readahead(uint32_t start_block, uint32_t count) {
    if (count == 0) {
        return;
    }

    if (count > 8) {
        count = 8;
    }

    for (uint32_t i = 1; i <= count; i++) {
        const uint32_t block_idx = start_block + i;

        BcacheShard& shard = g_shards[shard_index_for(block_idx)];
        {
            kernel::ReadGuard meta_guard(shard.meta_lock);

            if (shard.lookup_locked(block_idx)) {
                continue;
            }
        }

        BcacheEntry* e = shard.get_or_create(block_idx, false, nullptr);
        if (!e) {
            continue;
        }

        const uint32_t flags = e->flags.load(kernel::memory_order::acquire);
        if ((flags & k_flag_io_inflight) != 0u) {
            e->io_done.wait();
        }

        e->put();
    }
}