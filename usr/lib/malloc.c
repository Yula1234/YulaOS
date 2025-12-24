#include "../yula.h"

#define PAGE_SIZE 4096
#define ALIGNMENT 8
#define ALIGN(size) (((size) + (ALIGNMENT - 1)) & ~(ALIGNMENT - 1))

typedef struct block_header {
    uint32_t size;         
    uint32_t is_free;
    struct block_header* next;
    struct block_header* prev;
} block_header_t;

#define HEADER_SIZE sizeof(block_header_t)

static block_header_t* heap_head = 0;

static block_header_t* request_space(block_header_t* last, uint32_t size) {
    uint32_t total_size = ALIGN(size + HEADER_SIZE);
    
    uint32_t pages_to_request = (total_size + PAGE_SIZE - 1) / PAGE_SIZE;
    uint32_t actual_request_size = pages_to_request * PAGE_SIZE;

    block_header_t* block = (block_header_t*)sbrk(actual_request_size);
    if ((int)block == -1) return 0;

    block->size = actual_request_size - HEADER_SIZE;
    block->is_free = 0;
    block->next = 0;
    block->prev = last;

    if (last) last->next = block;
    return block;
}

static void split_block(block_header_t* block, uint32_t size) {
    if (block->size >= size + HEADER_SIZE + ALIGNMENT) {
        block_header_t* new_free_block = (block_header_t*)((uint8_t*)block + HEADER_SIZE + size);
        
        new_free_block->size = block->size - size - HEADER_SIZE;
        new_free_block->is_free = 1;
        new_free_block->next = block->next;
        new_free_block->prev = block;

        if (block->next) block->next->prev = new_free_block;
        
        block->next = new_free_block;
        block->size = size;
    }
}

void* malloc(uint32_t size) {
    if (size == 0) return 0;
    size = ALIGN(size);

    block_header_t* curr = heap_head;
    block_header_t* last = 0;

    while (curr) {
        if (curr->is_free && curr->size >= size) {
            split_block(curr, size);
            curr->is_free = 0;
            return (void*)(curr + 1);
        }
        last = curr;
        curr = curr->next;
    }

    curr = request_space(last, size);
    if (!curr) return 0;

    if (!heap_head) heap_head = curr;

    split_block(curr, size);
    return (void*)(curr + 1);
}

void free(void* ptr) {
    if (!ptr) return;

    block_header_t* curr = (block_header_t*)ptr - 1;
    curr->is_free = 1;

    if (curr->next && curr->next->is_free) {
        curr->size += HEADER_SIZE + curr->next->size;
        curr->next = curr->next->next;
        if (curr->next) curr->next->prev = curr;
    }

    if (curr->prev && curr->prev->is_free) {
        block_header_t* prev_node = curr->prev;
        prev_node->size += HEADER_SIZE + curr->size;
        prev_node->next = curr->next;
        if (curr->next) curr->next->prev = prev_node;
        curr = prev_node;
    }
}