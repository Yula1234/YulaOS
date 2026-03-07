/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2025 Yula1234 */

#include "bcache.h"
#include "../drivers/ahci.h"
#include "../lib/string.h"
#include "../hal/lock.h"
#include "../kernel/sched.h"
#include "../mm/heap.h"

#include "../lib/hash_map.h"

/*
 * Block cache.
 *
 * The cache provides a fixed-size in-memory store for 4KiB blocks indexed by
 * `block_idx`.
 *
 * Data structures:
 *  - block_map: lookup by block index
 *  - LRU list: eviction policy
 *
 * Concurrency model:
 *  - `cache_lock` protects all metadata and cached data buffers.
 *  - slow I/O is performed outside the lock.
 *  - the refill path re-checks the cache after I/O to handle races.
 *
 * The implementation is intentionally simple: there is no pinning, no per-block
 * locks, and no asynchronous writeback. Users must treat cache entries as
 * ephemeral and copy data out.
 */

#define BCACHE_SIZE     128
#define HASH_BUCKETS    64
#define BLOCK_SIZE      4096
#define SECTOR_SIZE     512
#define SECTORS_PER_BLK 8

typedef struct cache_block {
    uint32_t block_idx;
    uint8_t  data[BLOCK_SIZE];

    uint8_t  valid;
    uint8_t  dirty;

    struct cache_block *prev;
    struct cache_block *next;
} cache_block_t;

static cache_block_t pool[BCACHE_SIZE];
static HashMap<uint32_t, cache_block_t*, HASH_BUCKETS> block_map;

static cache_block_t* lru_head = 0;
static cache_block_t* lru_tail = 0;

static spinlock_t cache_lock;

/*
 * Disk I/O helpers.
 *
 * The cache operates on 4KiB blocks. AHCI reads/writes in 512-byte sectors, so
 * we translate block indices to LBA by multiplying by SECTORS_PER_BLK.
 */

static void disk_read_4k(uint32_t block_idx, uint8_t* buf) {
    if (!buf) {
        return;
    }

    uint64_t start_lba = (uint64_t)block_idx * (uint64_t)SECTORS_PER_BLK;
    if (start_lba > 0xFFFFFFFF) {
        return;
    }

    ahci_read_sectors((uint32_t)start_lba, SECTORS_PER_BLK, buf);
}

static void disk_write_4k(uint32_t block_idx, const uint8_t* buf) {
    if (!buf) {
        return;
    }

    uint64_t start_lba = (uint64_t)block_idx * (uint64_t)SECTORS_PER_BLK;
    if (start_lba > 0xFFFFFFFF) {
        return;
    }

    ahci_write_sectors((uint32_t)start_lba, SECTORS_PER_BLK, buf);
}

static void lru_touch(cache_block_t* b) {
    /* Move block to the MRU position. */
    if (b == lru_head) {
        return;
    }

    if (b->prev) {
        b->prev->next = b->next;
    }
    if (b->next) {
        b->next->prev = b->prev;
    }
    if (b == lru_tail) {
        lru_tail = b->prev;
    }

    b->next = lru_head;
    b->prev = 0;

    if (lru_head) {
        lru_head->prev = b;
    }

    lru_head = b;

    if (!lru_tail) {
        lru_tail = b;
    }
}

static void hash_remove(cache_block_t* b) {
    if (!b->valid) {
        return;
    }

    block_map.remove(b->block_idx);
}

static void hash_insert(cache_block_t* b) {
    block_map.insert_or_assign(b->block_idx, b);
}

static cache_block_t* cache_lookup(uint32_t block_idx) {
    cache_block_t* b = nullptr;
    if (!block_map.try_get(block_idx, b)) {
        return nullptr;
    }

    if (!b || !b->valid) {
        return nullptr;
    }

    return b;
}

void bcache_init(void) {
    /*
     * Initialize the fixed pool and build the initial LRU list.
     *
     * All blocks start invalid. The LRU list is seeded with the entire pool so
     * the tail is always a candidate for eviction.
     */
    memset(pool, 0, sizeof(pool));
    block_map.clear();

    spinlock_init(&cache_lock);

    lru_head = 0;
    lru_tail = 0;

    for (int i = 0; i < BCACHE_SIZE; i++) {
        cache_block_t* b = &pool[i];

        b->next = lru_head;
        b->prev = 0;

        if (lru_head) {
            lru_head->prev = b;
        }

        lru_head = b;

        if (!lru_tail) {
            lru_tail = b;
        }
    }
}

int bcache_read(uint32_t block_idx, uint8_t* buf) {
    if (!buf) {
        return 0;
    }

    spinlock_acquire(&cache_lock);

    cache_block_t* b = cache_lookup(block_idx);
    if (b) {
        /* Fast path: cached hit. */
        lru_touch(b);

        memcpy(buf, b->data, BLOCK_SIZE);

        spinlock_release(&cache_lock);
        return 1;
    }

    b = lru_tail;
    if (!b) {
        spinlock_release(&cache_lock);
        return 0;
    }

    /*
     * Eviction: copy dirty victim data out, drop from hash, then do I/O
     * without holding cache_lock.
     */

    int was_dirty = b->valid
                   && b->dirty;
    uint32_t old_idx = b->block_idx;
    uint8_t dirty_data[BLOCK_SIZE];

    if (was_dirty) {
        memcpy(dirty_data, b->data, BLOCK_SIZE);
    }

    hash_remove(b);
    spinlock_release(&cache_lock);

    if (was_dirty) {
        disk_write_4k(old_idx, dirty_data);
    }

    disk_read_4k(block_idx, buf);

    spinlock_acquire(&cache_lock);

    cache_block_t* race_check = cache_lookup(block_idx);
    if (race_check) {
        /*
         * Another CPU filled the same block while we were doing I/O.
         * Prefer the cached copy and keep our read buffer as the output.
         */
        lru_touch(race_check);

        memcpy(buf, race_check->data, BLOCK_SIZE);

        spinlock_release(&cache_lock);
        return 1;
    }

    b = lru_tail;
    if (!b) {
        spinlock_release(&cache_lock);
        return 0;
    }

    /*
     * Second-chance victim selection.
     *
     * We must re-select because `lru_tail` and the victim contents may have
     * changed while we were doing I/O.
     */

    int new_was_dirty = b->valid
                       && b->dirty;
    uint32_t new_old_idx = b->block_idx;
    uint8_t new_dirty_data[BLOCK_SIZE];

    if (new_was_dirty) {
        memcpy(new_dirty_data, b->data, BLOCK_SIZE);
    }

    hash_remove(b);

    b->block_idx = block_idx;
    b->valid = 1;
    b->dirty = 0;
    memcpy(b->data, buf, BLOCK_SIZE);

    hash_insert(b);
    lru_touch(b);

    spinlock_release(&cache_lock);

    if (new_was_dirty) {
        disk_write_4k(new_old_idx, new_dirty_data);
    }

    return 1;
}

int bcache_write(uint32_t block_idx, const uint8_t* buf) {
    if (!buf) {
        return 0;
    }

    spinlock_acquire(&cache_lock);
    cache_block_t* b = cache_lookup(block_idx);
    if (b) {
        /* Cached hit: update in place and mark dirty. */
        lru_touch(b);

        memcpy(b->data, buf, BLOCK_SIZE);

        b->dirty = 1;

        spinlock_release(&cache_lock);
        return 1;
    }

    b = lru_tail;
    if (!b) {
        spinlock_release(&cache_lock);
        return 0;
    }

    int was_dirty = b->valid
                   && b->dirty;
    uint32_t old_idx = b->block_idx;
    uint8_t dirty_data[BLOCK_SIZE];

    if (was_dirty) {
        memcpy(dirty_data, b->data, BLOCK_SIZE);
    }

    hash_remove(b);

    b->block_idx = block_idx;
    b->valid = 1;
    b->dirty = 1;
    memcpy(b->data, buf, BLOCK_SIZE);

    hash_insert(b);
    lru_touch(b);

    spinlock_release(&cache_lock);

    if (was_dirty) {
        disk_write_4k(old_idx, dirty_data);
    }

    return 1;
}

void bcache_sync(void) {
    /*
     * Write back dirty blocks.
     *
     * Each block is captured under the lock and flushed outside the lock to
     * avoid stalling concurrent lookups.
     */
    for (int i = 0; i < BCACHE_SIZE; i++) {
        uint8_t  tmp_buf[BLOCK_SIZE];
        uint32_t blk_idx = 0;
        int      do_flush = 0;

        spinlock_acquire(&cache_lock);
        cache_block_t* b = &pool[i];

        if (b->valid && b->dirty) {
            blk_idx = b->block_idx;
            memcpy(tmp_buf, b->data, BLOCK_SIZE);
            b->dirty = 0;
            do_flush = 1;
        }

        spinlock_release(&cache_lock);

        if (do_flush) {
            disk_write_4k(blk_idx, tmp_buf);
        }
    }
}

void bcache_flush_block(uint32_t block_idx) {
    /* Write back a single dirty block if it is cached. */
    uint8_t  tmp_buf[BLOCK_SIZE];
    uint32_t phys_idx = 0;
    int      do_flush = 0;

    spinlock_acquire(&cache_lock);
    cache_block_t* b = cache_lookup(block_idx);

    if (b && b->dirty) {
        phys_idx = b->block_idx;

        memcpy(tmp_buf, b->data, BLOCK_SIZE);

        b->dirty = 0;
        do_flush = 1;
    }

    spinlock_release(&cache_lock);

    if (do_flush) {
        disk_write_4k(phys_idx, tmp_buf);
    }
}

void bcache_readahead(uint32_t start_block, uint32_t count) {
    /*
     * Opportunistic sequential prefetch.
     *
     * This code trades optimality for simplicity: it does not reserve cache
     * entries and can race with real reads/writes. The only hard guarantee is
     * that it never blocks the caller on cache_lock during disk I/O.
     */
    if (count == 0) {
        return;
    }

    uint32_t max_prefetch = 8;
    if (count > max_prefetch) {
        count = max_prefetch;
    }

    for (uint32_t i = 1; i <= count; i++) {
        uint32_t block_idx = start_block + i;

        spinlock_acquire(&cache_lock);
        cache_block_t* b = cache_lookup(block_idx);
        if (b) {
            /* Already cached: just refresh LRU. */
            lru_touch(b);

            spinlock_release(&cache_lock);
            continue;
        }

        b = lru_tail;
        if (!b) {
            spinlock_release(&cache_lock);
            continue;
        }

        /* Evict one victim and do I/O without holding cache_lock. */

        int was_dirty = b->valid
                       && b->dirty;
        uint32_t old_idx = b->block_idx;
        uint8_t dirty_data[BLOCK_SIZE];

        if (was_dirty) {
            memcpy(dirty_data, b->data, BLOCK_SIZE);
        }

        hash_remove(b);
        spinlock_release(&cache_lock);

        if (was_dirty) {
            disk_write_4k(old_idx, dirty_data);
        }

        uint8_t* scratch = (uint8_t*)kmalloc(BLOCK_SIZE);
        if (!scratch) {
            continue;
        }

        disk_read_4k(block_idx, scratch);

        spinlock_acquire(&cache_lock);

        cache_block_t* race_check = cache_lookup(block_idx);
        if (race_check) {
            /* Another CPU brought it in meanwhile. */
            lru_touch(race_check);

            kfree(scratch);

            spinlock_release(&cache_lock);
            continue;
        }

        b = lru_tail;
        if (!b) {
            kfree(scratch);
            spinlock_release(&cache_lock);
            continue;
        }

        /* Re-select victim after I/O to avoid using stale state. */

        int new_was_dirty = b->valid
                           && b->dirty;

        uint32_t new_old_idx = b->block_idx;
        uint8_t new_dirty_data[BLOCK_SIZE];

        if (new_was_dirty) {
            memcpy(new_dirty_data, b->data, BLOCK_SIZE);
        }

        hash_remove(b);

        b->block_idx = block_idx;
        b->valid = 1;
        b->dirty = 0;

        memcpy(b->data, scratch, BLOCK_SIZE);

        hash_insert(b);
        lru_touch(b);

        spinlock_release(&cache_lock);

        if (new_was_dirty) {
            disk_write_4k(new_old_idx, new_dirty_data);
        }

        kfree(scratch);
    }
}