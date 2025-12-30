#include "bcache.h"
#include "../drivers/ahci.h"
#include "../lib/string.h"
#include "../hal/lock.h"
#include "../kernel/sched.h"

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
    struct cache_block *h_next;
    struct cache_block *h_prev;
} cache_block_t;

static cache_block_t pool[BCACHE_SIZE];
static cache_block_t* hash_table[HASH_BUCKETS];
static cache_block_t* lru_head = 0;
static cache_block_t* lru_tail = 0;
static spinlock_t cache_lock;

static inline uint32_t hash_idx(uint32_t idx) { return idx & (HASH_BUCKETS - 1); }

static void disk_read_4k(uint32_t block_idx, uint8_t* buf) {
    uint32_t start_lba = block_idx * SECTORS_PER_BLK;
    ahci_read_sectors(start_lba, SECTORS_PER_BLK, buf);
}

static void disk_write_4k(uint32_t block_idx, const uint8_t* buf) {
    uint32_t start_lba = block_idx * SECTORS_PER_BLK;
    ahci_write_sectors(start_lba, SECTORS_PER_BLK, buf);
}

static void lru_touch(cache_block_t* b) {
    if (b == lru_head) return; 
    if (b->prev) b->prev->next = b->next;
    if (b->next) b->next->prev = b->prev;
    if (b == lru_tail) lru_tail = b->prev;
    b->next = lru_head;
    b->prev = 0;
    if (lru_head) lru_head->prev = b;
    lru_head = b;
    if (!lru_tail) lru_tail = b;
}

static void hash_remove(cache_block_t* b) {
    if (!b->valid) return;
    if (b->h_prev) b->h_prev->h_next = b->h_next;
    else {
        uint32_t h = hash_idx(b->block_idx);
        if (hash_table[h] == b) hash_table[h] = b->h_next;
    }
    if (b->h_next) b->h_next->h_prev = b->h_prev;
    b->h_next = 0; b->h_prev = 0;
}

static void hash_insert(cache_block_t* b) {
    uint32_t h = hash_idx(b->block_idx);
    b->h_next = hash_table[h];
    b->h_prev = 0;
    if (hash_table[h]) hash_table[h]->h_prev = b;
    hash_table[h] = b;
}

static cache_block_t* cache_lookup(uint32_t block_idx) {
    uint32_t h = hash_idx(block_idx);
    cache_block_t* b = hash_table[h];
    while (b) {
        if (b->block_idx == block_idx && b->valid) return b;
        b = b->h_next;
    }
    return 0;
}

void bcache_init(void) {
    memset(pool, 0, sizeof(pool));
    memset(hash_table, 0, sizeof(hash_table));
    spinlock_init(&cache_lock);
    lru_head = 0; lru_tail = 0;
    for (int i = 0; i < BCACHE_SIZE; i++) {
        cache_block_t* b = &pool[i];
        b->next = lru_head; b->prev = 0;
        if (lru_head) lru_head->prev = b;
        lru_head = b;
        if (!lru_tail) lru_tail = b;
    }
}

int bcache_read(uint32_t block_idx, uint8_t* buf) {
    uint32_t flags = spinlock_acquire_safe(&cache_lock);
    cache_block_t* b = cache_lookup(block_idx);
    if (b) {
        lru_touch(b);
        memcpy(buf, b->data, BLOCK_SIZE);
        spinlock_release_safe(&cache_lock, flags);
        return 1;
    }
    b = lru_tail;
    if (!b) { spinlock_release_safe(&cache_lock, flags); return 0; }
    if (b->valid && b->dirty) {
        uint32_t old_idx = b->block_idx;
        disk_write_4k(old_idx, b->data);
        b->dirty = 0;
    }
    hash_remove(b);
    spinlock_release_safe(&cache_lock, flags);
    
    disk_read_4k(block_idx, buf);

    flags = spinlock_acquire_safe(&cache_lock);
    cache_block_t* race_check = cache_lookup(block_idx);
    if (race_check) {
        lru_touch(race_check);
        memcpy(buf, race_check->data, BLOCK_SIZE);
        spinlock_release_safe(&cache_lock, flags);
        return 1;
    }
    b = lru_tail;
    if (b->valid && b->dirty) {
        disk_write_4k(b->block_idx, b->data);
        b->dirty = 0;
    }
    hash_remove(b);
    b->block_idx = block_idx;
    b->valid = 1;
    b->dirty = 0;
    memcpy(b->data, buf, BLOCK_SIZE);
    hash_insert(b);
    lru_touch(b);
    spinlock_release_safe(&cache_lock, flags);
    return 1;
}

int bcache_write(uint32_t block_idx, const uint8_t* buf) {
    uint32_t flags = spinlock_acquire_safe(&cache_lock);
    cache_block_t* b = cache_lookup(block_idx);
    if (b) {
        lru_touch(b);
        memcpy(b->data, buf, BLOCK_SIZE);
        b->dirty = 1;
    } else {
        b = lru_tail;
        if (b->valid && b->dirty) {
            disk_write_4k(b->block_idx, b->data);
            b->dirty = 0;
        }
        hash_remove(b);
        b->block_idx = block_idx;
        b->valid = 1;
        b->dirty = 1;
        memcpy(b->data, buf, BLOCK_SIZE);
        hash_insert(b);
        lru_touch(b);
    }
    spinlock_release_safe(&cache_lock, flags);
    return 1;
}

void bcache_sync(void) {
    for (int i = 0; i < BCACHE_SIZE; i++) {
        cache_block_t* b = &pool[i];
        if (b->valid && b->dirty) {
            uint32_t flags = spinlock_acquire_safe(&cache_lock);
            if (b->valid && b->dirty) {
                disk_write_4k(b->block_idx, b->data);
                b->dirty = 0;
            }
            spinlock_release_safe(&cache_lock, flags);
        }
    }
}

void bcache_flush_block(uint32_t block_idx) {
    uint32_t flags = spinlock_acquire_safe(&cache_lock);
    cache_block_t* b = cache_lookup(block_idx);
    if (b && b->dirty) {
        disk_write_4k(b->block_idx, b->data);
        b->dirty = 0;
    }
    spinlock_release_safe(&cache_lock, flags);
}