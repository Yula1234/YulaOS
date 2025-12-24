#include <lib/string.h>
#include <hal/lock.h>

#include "pmm.h"

#define MAX_MEM 128*1024*1024
#define BLOCK_SIZE 4096
#define BITMAP_SIZE (MAX_MEM / BLOCK_SIZE / 8)

static uint8_t bitmap[BITMAP_SIZE];
static spinlock_t pmm_lock;

static uint32_t used_blocks = 0;
static uint32_t total_blocks = 0;

void pmm_init(uint32_t mem_size) {
    spinlock_init(&pmm_lock);

    total_blocks = mem_size / BLOCK_SIZE;
    used_blocks = total_blocks;
    
    memset(bitmap, 0xFF, BITMAP_SIZE);

    for (uint32_t addr = 0x1000000; addr < mem_size; addr += BLOCK_SIZE) {
        pmm_free_block((void*)addr);
    }
}

void* pmm_alloc_block() {
    uint32_t flags = spinlock_acquire_safe(&pmm_lock);

    for (uint32_t i = 0; i < BITMAP_SIZE; i++) {
        if (bitmap[i] != 0xFF) {
            for (int j = 0; j < 8; j++) {
                if (!(bitmap[i] & (1 << j))) {
                    bitmap[i] |= (1 << j);
                    used_blocks++;
                    spinlock_release_safe(&pmm_lock, flags);
                    return (void*)((i * 8 + j) * BLOCK_SIZE);
                }
            }
        }
    }
    
    spinlock_release_safe(&pmm_lock, flags);
    return 0; // Out of memory
}

void pmm_free_block(void* addr) {
    uint32_t flags = spinlock_acquire_safe(&pmm_lock);
    
    uint32_t block = (uint32_t)addr / BLOCK_SIZE;
    if (bitmap[block / 8] & (1 << (block % 8))) {
        bitmap[block / 8] &= ~(1 << (block % 8));
        if (used_blocks > 0) used_blocks--; 
    }
    
    spinlock_release_safe(&pmm_lock, flags);
}

uint32_t pmm_get_used_blocks() { return used_blocks; }
uint32_t pmm_get_free_blocks() { return total_blocks - used_blocks; }