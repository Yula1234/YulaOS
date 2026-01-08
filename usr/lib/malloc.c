// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#include <yula.h>

#define ALIGNMENT       8       
#define NUM_BINS        32      
#define BIN_STEP        16      
#define CHUNK_SIZE      65536   

#define BLOCK_MAGIC     0xDEADBEEF
#define BLOCK_USED      0
#define BLOCK_FREE      1

#define ALIGN(size) (((size) + (ALIGNMENT - 1)) & ~(ALIGNMENT - 1))

typedef struct BlockHeader {
    size_t size;                   
    struct BlockHeader* next_phys; 
    struct BlockHeader* prev_phys; 
    int is_free;                   
    uint32_t magic;                
} Block;

typedef struct FreeNode {
    struct FreeNode* next;
    struct FreeNode* prev;
} FreeNode;

#define HEADER_SIZE (ALIGN(sizeof(Block)))

#define MIN_BLOCK_SIZE (HEADER_SIZE + sizeof(FreeNode))

static FreeNode* bins[NUM_BINS];
static FreeNode* large_bin = NULL;
static Block* top_chunk = NULL;

static void panic(const char* msg, void* ptr) {
    print("\n[MALLOC ERROR] ");
    print(msg);
    print(" at 0x");
    print_hex((uint32_t)ptr);
    print("\n");
    exit(1);
}

static void* get_data_ptr(Block* block) {
    return (void*)((char*)block + HEADER_SIZE);
}

static Block* get_header(void* ptr) {
    return (Block*)((char*)ptr - HEADER_SIZE);
}

static int validate_block(Block* block) {
    return block && block->magic == BLOCK_MAGIC;
}

static int get_bin_index(size_t size) {
    size_t payload_size = size - HEADER_SIZE;
    if (payload_size < BIN_STEP) return 0;
    int idx = (payload_size / BIN_STEP) - 1;
    if (idx >= NUM_BINS) return -1;
    return idx;
}

static void insert_into_bin(Block* block) {
    FreeNode* node = (FreeNode*)get_data_ptr(block);
    int idx = get_bin_index(block->size);
    
    FreeNode** head_ptr = (idx == -1) ? &large_bin : &bins[idx];

    node->next = *head_ptr;
    node->prev = NULL;
    
    if (*head_ptr) {
        (*head_ptr)->prev = node;
    }
    *head_ptr = node;
}

static void remove_from_bin(Block* block) {
    FreeNode* node = (FreeNode*)get_data_ptr(block);
    int idx = get_bin_index(block->size);
    
    FreeNode** head_ptr = (idx == -1) ? &large_bin : &bins[idx];

    if (node->prev) {
        node->prev->next = node->next;
    } else {
        *head_ptr = node->next;
    }

    if (node->next) {
        node->next->prev = node->prev;
    }
}

static void extend_heap(size_t min_size) {
    size_t req_size = CHUNK_SIZE;
    
    if (min_size > req_size) {
        req_size = min_size;
    }
    

    req_size = (req_size + 4095) & ~4095;

    void* p = sbrk(req_size);
    
    if ((int)p == -1) return; // OOM

    Block* new_region = (Block*)p;
    new_region->size = req_size;
    new_region->magic = BLOCK_MAGIC;
    new_region->is_free = BLOCK_FREE;
    new_region->next_phys = NULL;
    
    if (top_chunk) {
        if (!top_chunk->is_free) panic("Heap corruption (top_chunk used)", top_chunk);
        char* top_end = (char*)top_chunk + top_chunk->size;
        if (top_end == (char*)new_region) {
            top_chunk->size += new_region->size;
            return; 
        }
        
        top_chunk->next_phys = new_region;
        new_region->prev_phys = top_chunk;
    } else {
        new_region->prev_phys = NULL;
    }
    
    top_chunk = new_region;
}

static Block* split_chunk(Block* block, size_t size) {
    if (block->size >= size + MIN_BLOCK_SIZE) {
        Block* remainder = (Block*)((char*)block + size);
        
        remainder->size = block->size - size;
        remainder->magic = BLOCK_MAGIC;
        remainder->is_free = BLOCK_FREE;
        
        remainder->next_phys = block->next_phys;
        remainder->prev_phys = block;
        
        if (remainder->next_phys) {
            remainder->next_phys->prev_phys = remainder;
        }
        
        block->size = size;
        block->next_phys = remainder;

        if (block == top_chunk) {
            top_chunk = remainder;
        } else {
            insert_into_bin(remainder);
        }
    }
    return block;
}

void* malloc(size_t size) {
    if (size == 0) return NULL;

    size_t aligned_size = ALIGN(size);
    size_t total_req = aligned_size + HEADER_SIZE;
    
    if (total_req < MIN_BLOCK_SIZE) total_req = MIN_BLOCK_SIZE;

    int start_bin = get_bin_index(total_req);
    if (start_bin != -1) {
        for (int i = start_bin; i < NUM_BINS; i++) {
            if (bins[i]) {
                FreeNode* node = bins[i];
                Block* block = get_header(node);
                remove_from_bin(block);
                block = split_chunk(block, total_req);
                block->is_free = BLOCK_USED;
                return get_data_ptr(block);
            }
        }
    }

    FreeNode* curr = large_bin;
    while (curr) {
        Block* block = get_header(curr);
        FreeNode* next_node = curr->next; 
        
        if (block->size >= total_req) {
            remove_from_bin(block);
            block = split_chunk(block, total_req);
            block->is_free = BLOCK_USED;
            return get_data_ptr(block);
        }
        curr = next_node;
    }

    if (!top_chunk || top_chunk->size < total_req + MIN_BLOCK_SIZE) {
        extend_heap(total_req + MIN_BLOCK_SIZE);
        if (!top_chunk || top_chunk->size < total_req + MIN_BLOCK_SIZE) return NULL;
    }

    Block* block = top_chunk;
    block = split_chunk(block, total_req);
    block->is_free = BLOCK_USED;
    
    return get_data_ptr(block);
}

void free(void* ptr) {
    if (!ptr) return;

    Block* block = get_header(ptr);
    if (!validate_block(block)) panic("Heap corruption", ptr);
    if (block->is_free) return;

    block->is_free = BLOCK_FREE;

    // Coalesce Right
    if (block->next_phys && block->next_phys->is_free) {
        Block* next = block->next_phys;
        if (next == top_chunk) {
            top_chunk = block;
        } else {
            remove_from_bin(next);
        }
        
        block->size += next->size;
        block->next_phys = next->next_phys;
        if (block->next_phys) block->next_phys->prev_phys = block;
    }

    // Coalesce Left
    if (block->prev_phys && block->prev_phys->is_free) {
        Block* prev = block->prev_phys;
        
        remove_from_bin(prev);
        
        if (block == top_chunk) {
            top_chunk = prev;
        }
        
        prev->size += block->size;
        prev->next_phys = block->next_phys;
        if (prev->next_phys) prev->next_phys->prev_phys = prev;
        
        block = prev;
    }

    if (block != top_chunk) {
        insert_into_bin(block);
    }
}

void* calloc(size_t nelem, size_t elsize) {
    size_t size = nelem * elsize;
    if (nelem != 0 && size / nelem != elsize) return NULL;
    void* ptr = malloc(size);
    if (ptr) memset(ptr, 0, size);
    return ptr;
}

void* realloc(void* ptr, size_t size) {
    if (!ptr) return malloc(size);
    if (size == 0) { free(ptr); return NULL; }

    Block* block = get_header(ptr);
    if (!validate_block(block)) panic("Heap corruption (realloc)", ptr);

    size_t new_total = ALIGN(size) + HEADER_SIZE;
    if (new_total < MIN_BLOCK_SIZE) new_total = MIN_BLOCK_SIZE;

    if (block->size >= new_total) return ptr;

    if (block->next_phys && block->next_phys->is_free) {
        Block* next = block->next_phys;
        size_t combined = block->size + next->size;
        if (combined >= new_total) {
            if (next == top_chunk) {
                if (combined < new_total + MIN_BLOCK_SIZE) {
                    size_t needed = (new_total + MIN_BLOCK_SIZE) - combined;
                    size_t page_aligned = (needed + 4095) & ~4095;
                    void* p = sbrk(page_aligned);
                    if ((int)p == -1) goto slow;
                    next->size += page_aligned;
                    combined += page_aligned;
                }
            } else {
                remove_from_bin(next);
            }

            block->size = combined;
            block->next_phys = next->next_phys;
            if (block->next_phys) block->next_phys->prev_phys = block;

            if (next == top_chunk) {
                top_chunk = block;
                block = split_chunk(block, new_total);
            }

            return ptr;
        }
    }

    if (block == top_chunk) {
        size_t needed = new_total - block->size;
        size_t page_aligned = (needed + 4095) & ~4095; 
        if ((int)sbrk(page_aligned) != -1) {
            block->size += page_aligned;
            block = split_chunk(block, new_total);
            return ptr;
        }
    }

slow:
    void* new_ptr = malloc(size);
    if (!new_ptr) return NULL;
    memcpy(new_ptr, ptr, block->size - HEADER_SIZE);
    free(ptr);
    return new_ptr;
}