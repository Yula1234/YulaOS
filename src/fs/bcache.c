#include "bcache.h"
#include "../drivers/ahci.h"
#include "../lib/string.h"
#include "../hal/lock.h"
#include "../kernel/sched.h"

#define BCACHE_SIZE     2048    // 1MB cache (2048 * 512)
#define BLOCK_SIZE      512

typedef struct {
    uint32_t lba;
    uint8_t  data[BLOCK_SIZE];
    uint32_t last_access;
    uint8_t  valid;
    uint8_t  dirty;
} cache_block_t;

static cache_block_t cache[BCACHE_SIZE];
static spinlock_t cache_lock;
static uint32_t access_counter = 0;

void bcache_init(void) {
    memset(cache, 0, sizeof(cache));
    spinlock_init(&cache_lock);
    access_counter = 0;
}

static int find_block_index(uint32_t lba) {
    for (int i = 0; i < BCACHE_SIZE; i++) {
        if (cache[i].valid && cache[i].lba == lba) return i;
    }
    return -1;
}

static int get_eviction_index(void) {
    for (int i = 0; i < BCACHE_SIZE; i++) if (!cache[i].valid) return i;
    uint32_t min_access = 0xFFFFFFFF;
    int candidate = 0;
    for (int i = 0; i < BCACHE_SIZE; i++) {
        if (cache[i].last_access < min_access) {
            min_access = cache[i].last_access;
            candidate = i;
        }
    }
    return candidate;
}

static void flush_slot(int idx) {
    if (cache[idx].valid && cache[idx].dirty) {
        ahci_write_sector(cache[idx].lba, cache[idx].data);
        cache[idx].dirty = 0;
    }
}

int bcache_read(uint32_t lba, uint8_t* buf) {
    uint32_t flags = spinlock_acquire_safe(&cache_lock);
    access_counter++;
    int idx = find_block_index(lba);
    if (idx != -1) {
        memcpy(buf, cache[idx].data, BLOCK_SIZE);
        cache[idx].last_access = access_counter;
        spinlock_release_safe(&cache_lock, flags);
        return 1;
    }
    int new_idx = get_eviction_index();
    flush_slot(new_idx);
    if (!ahci_read_sector(lba, cache[new_idx].data)) {
        spinlock_release_safe(&cache_lock, flags);
        return 0;
    }
    cache[new_idx].lba = lba;
    cache[new_idx].valid = 1;
    cache[new_idx].dirty = 0;
    cache[new_idx].last_access = access_counter;
    memcpy(buf, cache[new_idx].data, BLOCK_SIZE);
    spinlock_release_safe(&cache_lock, flags);
    return 1;
}

int bcache_write(uint32_t lba, const uint8_t* buf) {
    uint32_t flags = spinlock_acquire_safe(&cache_lock);
    access_counter++;
    int idx = find_block_index(lba);
    if (idx == -1) {
        idx = get_eviction_index();
        flush_slot(idx);
        cache[idx].lba = lba;
        cache[idx].valid = 1;
    }
    memcpy(cache[idx].data, buf, BLOCK_SIZE);
    cache[idx].dirty = 1;
    cache[idx].last_access = access_counter;
    spinlock_release_safe(&cache_lock, flags);
    return 1;
}

void bcache_sync(void) {
    for (int i = 0; i < BCACHE_SIZE; i++) {
        uint32_t flags = spinlock_acquire_safe(&cache_lock);
        
        if (cache[i].valid && cache[i].dirty) {
            uint32_t lba = cache[i].lba;
            uint8_t temp_buf[BLOCK_SIZE];
            memcpy(temp_buf, cache[i].data, BLOCK_SIZE);
            
            ahci_write_sector(lba, cache[i].data);
            cache[i].dirty = 0;
        }
        
        spinlock_release_safe(&cache_lock, flags);
    
        if (i % 10 == 0) sched_yield(); 
    }
}

void bcache_flush_block(uint32_t lba) {
    uint32_t flags = spinlock_acquire_safe(&cache_lock);
    int idx = find_block_index(lba);
    if (idx != -1) flush_slot(idx);
    spinlock_release_safe(&cache_lock, flags);
}