/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2025 Yula1234 */

#include <lib/cpp/hash_traits.h>
#include <lib/cpp/atomic.h>
#include <lib/cpp/dlist.h>
#include <lib/cpp/lock_guard.h>
#include <lib/cpp/new.h>
#include <lib/cpp/semaphore.h>

#include <lib/hash_map.h>
#include <lib/string.h>

#include <kernel/sched.h>
#include <kernel/smp/cpu.h>
#include <kernel/proc.h>

#include <mm/heap.h>
#include <mm/pmm.h>

#include <drivers/block/bdev.h>
#include <hal/lock.h>

#include "bcache.h"

/*
 * Block cache design notes.
 *
 * This cache sits below the filesystem and intentionally exposes a synchronous
 * API. Do not add pinning semantics implicitly: callers must assume that any
 * cached block can be evicted right after a call returns.
 *
 * Concurrency is split by intent:
 * - shard metadata is protected by per-shard meta_lock
 * - block payload is protected by per-entry data_lock
 * - the hot-cache is per-CPU and exists purely to avoid shard lock traffic
 *
 * Keep this file readable. Any future feature that changes the lifetime rules,
 * lock ordering or eviction invariants must be documented next to the code.
 */

static constexpr uint32_t BLOCK_SIZE = 4096;
static constexpr uint32_t SECTOR_SIZE = 512;
static constexpr uint32_t SECTORS_PER_BLK = 8;

static constexpr uint32_t BCACHE_SHARDS = 64;
static constexpr uint32_t BCACHE_SHARD_BUCKETS = 256;

static constexpr uint32_t HOT_SLOTS = 64;

/*
 * flags is deliberately a bitmask instead of an enum class.
 *
 * This is hot code that uses atomic read-modify-write. Keep the operations
 * explicit and cheap; avoid layering abstractions here.
 */
static constexpr uint32_t k_flag_valid = 1u << 0;
static constexpr uint32_t k_flag_dirty = 1u << 1;
static constexpr uint32_t k_flag_io_inflight = 1u << 2;
static constexpr uint32_t k_flag_evicting = 1u << 3;
static constexpr uint32_t k_flag_accessed = 1u << 4;
static constexpr uint32_t k_flag_on_dirty_list = 1u << 5;

static kmem_cache_t* g_entry_cache = nullptr;

struct BcacheIoEvent {
    /*
     * Avoid blocking on a spin-guarded condition.
     *
     * The atomic guards against lost wakeups and allows waiters to re-check the
     * condition without holding locks.
     */
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
    /*
     * Lifetime rules are explicit because eviction is concurrent with users.
     *
     * Keep exactly one long-lived reference while the entry is linked in the
     * shard map; everything else must be a short-lived get()/put(). Freeing is
     * gated by k_flag_evicting so that an entry cannot disappear from under a
     * reader that already holds a reference.
     */
    uint32_t block_idx = 0;

    kernel::atomic<uint32_t> refcnt{0};
    kernel::atomic<uint32_t> flags{0};

    rwspinlock_t data_lock{};
    BcacheIoEvent io_done{};

    uint8_t* data = nullptr;

    BcacheEntry* clock_next = nullptr;
    BcacheEntry* clock_prev = nullptr;

    dlist_head_t dirty_node{};

    BcacheEntry() {
        rwspinlock_init(&data_lock);
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

    bool try_get_not_evicting() {
        const uint32_t f0 = flags.load(kernel::memory_order::acquire);
        if ((f0 & k_flag_evicting) != 0u) {
            return false;
        }

        get();

        const uint32_t f1 = flags.load(kernel::memory_order::acquire);
        if ((f1 & k_flag_evicting) != 0u) {
            put();
            return false;
        }

        return true;
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

    /*
     * Only store a single direct pointer per slot.
     *
     * The cache is intentionally lossy: misses are acceptable, correctness is
     * not. Any policy more complex than this belongs in the shard.
     */

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

        /*
         * Refuse to publish an entry that is already on the eviction path.
         * That check must be done before taking the hot-cache lock; otherwise
         * the eviction side can free the object while it is being installed.
         */
        if (!entry.try_get_not_evicting()) {
            return;
        }

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

private:
    static uint32_t slot_index_for(uint32_t block_idx) {
        return kernel::HashTraits<uint32_t>::hash(block_idx)
            & (HOT_SLOTS - 1u);
    }
};

struct HotCache {
    /*
     * Per-CPU fast path exists to avoid taking shard locks on hot blocks.
     *
     * Store only additionally referenced entries here; otherwise eviction would
     * race and free memory that a CPU still points at.
     */
    PerCpuHotCache per_cpu[MAX_CPUS]{};

    void init() {
        for (int cpu = 0; cpu < cpu_count; cpu++) {
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
        const uint32_t entry_flags =
            entry.flags.load(kernel::memory_order::acquire);
        if ((entry_flags & k_flag_evicting) != 0u) {
            return;
        }

        const int cpu_idx = cpu_index();
        if (cpu_idx < 0) {
            return;
        }

        per_cpu[cpu_idx].put(block_idx, entry);
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

static constexpr uint32_t PREFETCH_QUEUE_CAP = 128;

struct PrefetchQueue {
    /*
     * Readahead must never stall the caller.
     *
     * Make the producer side drop work on pressure instead of sleeping.
     * The semaphore exists only to park the worker when the ring is empty; the
     * spinlock protects the ring metadata and nothing else.
     */
    kernel::SpinLock lock{};
    kernel::Semaphore sem{};

    BcacheEntry* items[PREFETCH_QUEUE_CAP]{};

    uint32_t head = 0;
    uint32_t tail = 0;
    uint32_t count = 0;

    bool try_push(BcacheEntry& e) {
        {
            kernel::SpinLockSafeGuard guard(lock);

            if (count == PREFETCH_QUEUE_CAP) {
                return false;
            }

            items[tail] = &e;
            tail = (tail + 1u) % PREFETCH_QUEUE_CAP;
            count++;
        }

        sem.signal();
        return true;
    }

    BcacheEntry* pop_blocking() {
        sem.wait();

        kernel::SpinLockSafeGuard guard(lock);

        if (count == 0) {
            return nullptr;
        }

        BcacheEntry* e = items[head];
        items[head] = nullptr;

        head = (head + 1u) % PREFETCH_QUEUE_CAP;
        count--;

        return e;
    }
};

static PrefetchQueue g_prefetch{};
static kernel::atomic<int> g_prefetch_started{0};

static block_device_t* g_bcache_bdev = nullptr;

extern "C" void bcache_attach_device(block_device_t* dev) {
    if (g_bcache_bdev == dev) return;

    if (g_bcache_bdev) {
        bdev_release(g_bcache_bdev);
    }

    g_bcache_bdev = dev;

    if (g_bcache_bdev) {
        bdev_retain(g_bcache_bdev);
    }
}

static void bcache_prefetch_worker(void*);

static bool ensure_prefetch_worker_started() {
    /*
     * Avoid spawning threads from early boot / no-process context.
     * Readahead is explicitly best-effort; return failure and let the caller
     * continue on the synchronous path.
     */
    if (g_prefetch_started.load(kernel::memory_order::acquire) != 0) {
        return true;
    }

    if (!proc_current()) {
        return false;
    }

    int expected = 0;

    /*
     * Serialize thread creation.
     *
     * A racing starter is not an error; treat it as success and let the other
     * CPU own the spawn path.
     */
    if (!g_prefetch_started.compare_exchange_strong(
            expected,
            1,
            kernel::memory_order::acq_rel,
            kernel::memory_order::acquire
        )) {
        return true;
    }

    /* If this ever turns into a start/stop circus, you're already fucked. */

    task_t* t = proc_spawn_kthread(
        "bcache_pf",
        PRIO_LOW,
        bcache_prefetch_worker,
        nullptr
    );

    if (!t) {
        /*
         * Drop the started flag on failure.
         *
         * Keeping it set would permanently disable readahead without providing
         * any diagnostic. A later caller may succeed.
         */
        g_prefetch_started.store(0, kernel::memory_order::release);
        return false;
    }

    return true;
}

struct DiskIo {
    /*
     * Keep the sector math and safety checks in one place.
     *
     * AHCI takes a 32-bit LBA; reject out-of-range block indices instead of
     * silently truncating and corrupting data.
     */
    static bool try_read_4k(uint32_t block_idx, uint8_t* buf) {
        if (!buf) {
            return false;
        }

        if (!g_bcache_bdev) {
            return false;
        }

        if (g_bcache_bdev->sector_size != SECTOR_SIZE) {
            return false;
        }

        const uint64_t start_lba =
            (uint64_t)block_idx * (uint64_t)SECTORS_PER_BLK;
        if (start_lba > 0xFFFFFFFF) {
            return false;
        }

        return bdev_read_sectors(g_bcache_bdev, start_lba, SECTORS_PER_BLK, buf) != 0;
    }

    static bool try_write_4k(uint32_t block_idx, const uint8_t* buf) {
        if (!buf) {
            return false;
        }

        if (!g_bcache_bdev) {
            return false;
        }

        if (g_bcache_bdev->sector_size != SECTOR_SIZE) {
            return false;
        }

        const uint64_t start_lba =
            (uint64_t)block_idx * (uint64_t)SECTORS_PER_BLK;
        if (start_lba > 0xFFFFFFFF) {
            return false;
        }

        return bdev_write_sectors(g_bcache_bdev, start_lba, SECTORS_PER_BLK, buf) != 0;
    }

    static void read_4k(uint32_t block_idx, uint8_t* buf) {
        (void)try_read_4k(block_idx, buf);
    }

    static void write_4k(uint32_t block_idx, const uint8_t* buf) {
        (void)try_write_4k(block_idx, buf);
    }
};

struct EvictedBlock {
    uint32_t block_idx = 0;

    BcacheEntry* entry = nullptr;

    bool dirty = false;
    uint8_t data[BLOCK_SIZE]{};

    /*
     * Keep a private copy of evicted dirty data.
     *
     * An entry cannot be written back after it is unlinked unless its payload
     * is captured while the data lock is still held.
     */

    bool has_entry() const {
        return entry != nullptr;
    }

    void clear() {
        *this = {};
    }

    void release_entry() {
        if (!entry) {
            return;
        }

        entry->put();
        entry = nullptr;
    }
};

struct BcacheShard {
    /*
     * Split locks by responsibility to keep fast paths short.
     *
     * meta_lock is for global shard structures (map/CLOCK/dirty_list). Data
     * lives under entry->data_lock. When both are required, take meta_lock
     * first, otherwise a writer can deadlock against a flusher.
     */
    rwspinlock_t meta_lock{};
    HashMap<uint32_t, BcacheEntry*, BCACHE_SHARD_BUCKETS> map{};

    kernel::CDBLinkedList<BcacheEntry, &BcacheEntry::dirty_node> dirty_list{};

    /*
     * dirty_list is an optimization, not just convenience.
     *
     * bcache_sync must be O(number of dirty blocks), not O(cache size). Keep
     * k_flag_on_dirty_list consistent with actual list membership.
     */

    BcacheEntry* clock_hand = nullptr;
    uint32_t entries = 0;
    uint32_t max_entries = 0;

    static uint32_t index_for(uint32_t block_idx) {
        return kernel::HashTraits<uint32_t>::hash(block_idx)
            & (BCACHE_SHARDS - 1u);
    }

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

    void dirty_mark_locked(BcacheEntry& e) {
        const uint32_t f = e.flags.load(kernel::memory_order::acquire);
        if ((f & k_flag_evicting) != 0u) {
            return;
        }

        if ((f & k_flag_on_dirty_list) != 0u) {
            return;
        }

        dirty_list.push_back(e);

        e.flags.fetch_or(k_flag_on_dirty_list, kernel::memory_order::acq_rel);
    }

    void dirty_unmark_locked(BcacheEntry& e) {
        const uint32_t f = e.flags.load(kernel::memory_order::acquire);
        if ((f & k_flag_on_dirty_list) == 0u) {
            return;
        }

        dlist_remove_node_if_present(dirty_list.native_head(), &e.dirty_node);

        e.flags.fetch_and(~k_flag_on_dirty_list, kernel::memory_order::acq_rel);
    }

    bool try_evict_one_locked(EvictedBlock& out) {
        /*
         * Eviction must not take anything away from an active user.
         *
         * Only consider refcnt==1 entries (the shard's own reference). The
         * accessed bit provides a cheap second chance under contention. Once
         * k_flag_evicting is visible, treat the entry as dead and unlink it from
         * every structure before dropping the shard reference.
         */
        if (!clock_hand) {
            return false;
        }

        for (uint32_t i = 0; i < entries + 1u; i++) {
            BcacheEntry* cand = clock_hand;
            clock_hand = clock_hand->clock_next;

            const uint32_t refs = cand->refcnt.load(kernel::memory_order::acquire);
            if (refs != 1u) {
                /*
                 * Do not evict from under an active user.
                 * refcnt>1 means somebody holds a live reference.
                 */
                continue;
            }

            uint32_t flags = cand->flags.load(kernel::memory_order::acquire);
            if ((flags & k_flag_accessed) != 0u) {
                /*
                 * Second chance.
                 * Clear accessed and move on; the next pass can evict if it is
                 * still cold.
                 */
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

            const uint32_t refs_after =
                cand->refcnt.load(kernel::memory_order::acquire);
            if (refs_after != 1u) {
                /*
                 * Close the race with a late get().
                 * Back out if somebody grabbed a reference after we checked.
                 */
                cand->flags.fetch_and(
                    ~k_flag_evicting,
                    kernel::memory_order::acq_rel
                );

                continue;
            }

            dirty_unmark_locked(*cand);

            map.remove(cand->block_idx);
            clock_remove_locked(*cand);
            entries--;

            /*
             * From here on the entry is unreachable through the shard.
             * Only existing references may still access it.
             */

            out.block_idx = cand->block_idx;
            out.entry = cand;

            flags = cand->flags.load(kernel::memory_order::acquire);
            if ((flags & k_flag_dirty) != 0u) {
                kernel::RwSpinLockNativeReadGuard data_guard(cand->data_lock);

                if (cand->data) {
                    memcpy(out.data, cand->data, BLOCK_SIZE);
                }

                out.dirty = true;

                /*
                 * Clear dirty after snapshot.
                 * A flusher must never re-emit the same payload twice.
                 */
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
        /*
         * Do not allocate an entry if making room fails.
         *
         * A cache that cannot evict is still required to behave correctly: the
         * public API is allowed to fall back to direct I/O or fail.
         */
        while (entries >= max_entries && max_entries != 0u) {
            EvictedBlock ev{};

            {
                kernel::RwSpinLockNativeWriteGuard meta_guard(meta_lock);

                if (entries < max_entries) {
                    break;
                }

                if (!try_evict_one_locked(ev)) {
                    return false;
                }
            }

            if (ev.dirty) {
                /*
                 * Writeback happens after dropping meta_lock.
                 * Keeping the lock over disk I/O would stall unrelated hits.
                 */

                /* Do not hold a lock while poking the disk. That's how you earn shitty latency. */
                DiskIo::write_4k(ev.block_idx, ev.data);
            }

            ev.release_entry();
        }

        return true;
    }

    BcacheEntry* create_readahead_entry(uint32_t block_idx) {
        /*
         * Never start I/O under meta_lock.
         * Readahead is a background activity and must keep shard lock hold time
         * minimal.
         */
        {
            kernel::RwSpinLockNativeReadGuard meta_guard(meta_lock);

            if (lookup_locked(block_idx)) {
                return nullptr;
            }
        }

        if (!ensure_space()) {
            return nullptr;
        }

        BcacheEntry* created = nullptr;

        {
            kernel::RwSpinLockNativeWriteGuard meta_guard(meta_lock);

            if (lookup_locked(block_idx)) {
                return nullptr;
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

            /*
             * Two references are required here.
             *
             * One stays with the shard map. The other is consumed by the
             * readahead pipeline (enqueue + worker) so the entry cannot be
             * reclaimed before I/O completion.
             */
            created->refcnt.store(2u, kernel::memory_order::release);
            created->flags.store(k_flag_io_inflight, kernel::memory_order::release);

            created->data = static_cast<uint8_t*>(kmalloc_a(BLOCK_SIZE));
            if (!created->data) {
                /*
                 * Put the entry on the eviction path before dropping the last
                 * reference; otherwise another CPU could observe it partially
                 * constructed.
                 */
                created->flags.fetch_or(
                    k_flag_evicting,
                    kernel::memory_order::acq_rel
                );

                created->refcnt.store(1u, kernel::memory_order::release);
                created->put();

                return nullptr;
            }

            created->io_done.reset();

            map.insert_or_assign(block_idx, created);
            clock_insert_locked(*created);

            entries++;
        }

        return created;
    }

    BcacheEntry* get_or_create(
        uint32_t block_idx,
        bool for_write,
        const uint8_t* write_buf
    ) {
        /*
         * Ensure the caller sees a single shared entry.
         *
         * First check under read lock so that hits never block each other.
         * The write lock phase repeats the lookup to avoid creating
         * two entries for the same block.
         */
        {
            kernel::RwSpinLockNativeReadGuard meta_guard(meta_lock);

            BcacheEntry* e = lookup_locked(block_idx);
            if (e) {
                if (e->try_get_not_evicting()) {
                    return e;
                }
            }
        }

        if (!ensure_space()) {
            return nullptr;
        }

        BcacheEntry* created = nullptr;

        {
            kernel::RwSpinLockNativeWriteGuard meta_guard(meta_lock);

            BcacheEntry* race = lookup_locked(block_idx);
            if (race) {
                if (race->try_get_not_evicting()) {
                    return race;
                }
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

            /*
             * for_write starts as valid+dirty so writers never block on I/O.
             * for_read starts inflight so concurrent readers can wait on a
             * single disk transaction.
             */
            created->flags.store(base_flags, kernel::memory_order::release);

            created->data = static_cast<uint8_t*>(kmalloc_a(BLOCK_SIZE));
            if (!created->data) {
                /*
                 * Mark evicting before dropping references.
                 * Leaving a visible entry without backing storage invites
                 * use-after-free and corrupt data paths.
                 */
                created->flags.fetch_or(
                    k_flag_evicting,
                    kernel::memory_order::acq_rel
                );

                created->refcnt.store(1u, kernel::memory_order::release);

                created->put();

                return nullptr;
            }

            if (for_write) {
                kernel::RwSpinLockNativeWriteGuard data_guard(created->data_lock);

                /*
                 * Copy the payload while the entry is still private.
                 * No other CPU can observe this block until it is linked.
                 */
                memcpy(created->data, write_buf, BLOCK_SIZE);

                dirty_mark_locked(*created);
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

        /*
         * Perform disk I/O outside meta_lock.
         * Holding the shard writer lock across AHCI I/O would serialize the
         * whole shard on a slow path.
         *
         * Read directly into the cache buffer. The entry is protected by
         * k_flag_io_inflight; no other thread can access it until we call
         * io_done.signal_all().
         */
        DiskIo::read_4k(block_idx, created->data);

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

static void bcache_prefetch_worker(void*) {
    for (;;) {
        BcacheEntry* e = g_prefetch.pop_blocking();
        if (!e) {
            continue;
        }

        /*
         * Read directly into the cache buffer.
         * The entry is protected by k_flag_io_inflight; no other thread can
         * access it until we call io_done.signal_all().
         */
        DiskIo::read_4k(e->block_idx, e->data);

        e->flags.fetch_or(
            k_flag_valid | k_flag_accessed,
            kernel::memory_order::acq_rel
        );

        e->flags.fetch_and(
            ~k_flag_io_inflight,
            kernel::memory_order::acq_rel
        );

        e->io_done.signal_all();

        e->put();
    }
}

void bcache_init(void) {
    if (!g_entry_cache) {
        g_entry_cache = kmem_cache_create("bcache_e", sizeof(BcacheEntry), 0, 0);
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
        rwspinlock_init(&g_shards[i].meta_lock);

        kernel::RwSpinLockNativeWriteGuard meta_guard(g_shards[i].meta_lock);

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
            kernel::RwSpinLockNativeReadGuard data_guard(hot->data_lock);

            memcpy(buf, hot->data, BLOCK_SIZE);
        }

        hot->flags.fetch_or(k_flag_accessed, kernel::memory_order::acq_rel);

        hot->put();
        return 1;
    }

    BcacheShard& shard = g_shards[BcacheShard::index_for(block_idx)];

    BcacheEntry* e = shard.get_or_create(block_idx, false, nullptr);
    if (!e) {
        return DiskIo::try_read_4k(block_idx, buf) ? 1 : 0;
    }

    const uint32_t flags = e->flags.load(kernel::memory_order::acquire);
    if ((flags & k_flag_io_inflight) != 0u) {
        e->io_done.wait();
    }

    {
        kernel::RwSpinLockNativeReadGuard data_guard(e->data_lock);

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
            kernel::RwSpinLockNativeWriteGuard data_guard(hot->data_lock);

            memcpy(hot->data, buf, BLOCK_SIZE);
        }

        const uint32_t prev_flags = hot->flags.fetch_or(
            k_flag_dirty | k_flag_valid | k_flag_accessed,
            kernel::memory_order::acq_rel
        );

        if ((prev_flags & k_flag_dirty) == 0u) {
            BcacheShard& shard = g_shards[BcacheShard::index_for(block_idx)];

            kernel::RwSpinLockNativeWriteGuard meta_guard(shard.meta_lock);

            shard.dirty_mark_locked(*hot);
        }

        hot->put();
        return 1;
    }

    BcacheShard& shard = g_shards[BcacheShard::index_for(block_idx)];

    BcacheEntry* e = shard.get_or_create(block_idx, true, buf);
    if (!e) {
        return 0;
    }

    const uint32_t flags = e->flags.load(kernel::memory_order::acquire);
    if ((flags & k_flag_io_inflight) != 0u) {
        e->io_done.wait();
    }

    {
        kernel::RwSpinLockNativeWriteGuard data_guard(e->data_lock);

        memcpy(e->data, buf, BLOCK_SIZE);
    }

    const uint32_t prev_flags = e->flags.fetch_or(
        k_flag_dirty | k_flag_valid | k_flag_accessed,
        kernel::memory_order::acq_rel
    );

    if ((prev_flags & k_flag_dirty) == 0u) {
        kernel::RwSpinLockNativeWriteGuard meta_guard(shard.meta_lock);

        shard.dirty_mark_locked(*e);
    }

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
                kernel::RwSpinLockNativeWriteGuard meta_guard(shard.meta_lock);

                if (shard.dirty_list.empty()) {
                    break;
                }

                BcacheEntry& cur = shard.dirty_list.front();

                shard.dirty_unmark_locked(cur);

                const uint32_t flags =
                    cur.flags.load(kernel::memory_order::acquire);
                if ((flags & k_flag_dirty) == 0u
                    || (flags & k_flag_evicting) != 0u) {
                    continue;
                }

                cur.get();

                cur.flags.fetch_and(
                    ~k_flag_dirty,
                    kernel::memory_order::acq_rel
                );

                {
                    kernel::RwSpinLockNativeReadGuard data_guard(cur.data_lock);

                    memcpy(tmp, cur.data, BLOCK_SIZE);
                }

                blk = cur.block_idx;
                e = &cur;
                do_flush = true;
            }

            if (do_flush) {
                DiskIo::write_4k(blk, tmp);
            }

            if (e) {
                e->put();
            }
        }
    }
}

void bcache_flush_block(uint32_t block_idx) {
    BcacheShard& shard = g_shards[BcacheShard::index_for(block_idx)];

    uint8_t tmp[BLOCK_SIZE];
    bool do_flush = false;

    BcacheEntry* e = nullptr;

    {
        kernel::RwSpinLockNativeWriteGuard meta_guard(shard.meta_lock);

        e = shard.lookup_locked(block_idx);
        if (!e) {
            return;
        }

        e->get();

        const uint32_t flags = e->flags.load(kernel::memory_order::acquire);
        if ((flags & k_flag_dirty) == 0u) {
            e->put();
            return;
        }

        shard.dirty_unmark_locked(*e);

        e->flags.fetch_and(
            ~k_flag_dirty,
            kernel::memory_order::acq_rel
        );
    }

    {
        kernel::RwSpinLockNativeReadGuard data_guard(e->data_lock);

        memcpy(tmp, e->data, BLOCK_SIZE);
    }

    do_flush = true;

    e->put();

    if (do_flush) {
        DiskIo::write_4k(block_idx, tmp);
    }
}

void bcache_readahead(uint32_t start_block, uint32_t count) {
    if (count == 0) {
        return;
    }

    if (!ensure_prefetch_worker_started()) {
        return;
    }

    if (count > 8) {
        count = 8;
    }

    for (uint32_t i = 1; i <= count; i++) {
        const uint32_t block_idx = start_block + i;

        BcacheShard& shard = g_shards[BcacheShard::index_for(block_idx)];

        BcacheEntry* e = shard.create_readahead_entry(block_idx);
        if (!e) {
            continue;
        }

        if (!g_prefetch.try_push(*e)) {
            BcacheShard& rollback_shard =
                g_shards[BcacheShard::index_for(e->block_idx)];

            bool unlinked = false;

            {
                kernel::RwSpinLockNativeWriteGuard meta_guard(rollback_shard.meta_lock);

                BcacheEntry* cur = rollback_shard.lookup_locked(e->block_idx);
                if (cur == e) {
                    e->flags.fetch_or(
                        k_flag_evicting,
                        kernel::memory_order::acq_rel
                    );

                    rollback_shard.map.remove(e->block_idx);
                    rollback_shard.clock_remove_locked(*e);
                    rollback_shard.entries--;

                    unlinked = true;
                }
            }

            e->put();

            if (unlinked) {
                /*
                 * Drop the shard's reference after unlink.
                 * The first put drops the creator's transient reference.
                 */
                e->put();
            }
        }
    }
}