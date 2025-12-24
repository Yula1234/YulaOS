#include "heap.h"
#include "../arch/i386/paging.h"
#include "../mm/pmm.h"
#include "../lib/string.h"
#include "../drivers/vga.h"
#include "../hal/lock.h"

#define HEAP_START_ADDR 0xD0000000
#define PAGE_SIZE 4096

typedef struct heap_header {
    size_t size;     
    uint8_t is_free; 
    struct heap_header* next;
} heap_header_t;

static heap_header_t* head = 0;
uint32_t heap_current_limit = HEAP_START_ADDR;
static spinlock_t heap_lock;

static int heap_expand(size_t size_needed) {
    size_t total_needed = size_needed + sizeof(heap_header_t);
    size_t pages = (total_needed + PAGE_SIZE - 1) / PAGE_SIZE;

    for (size_t i = 0; i < pages; i++) {
        void* phys = pmm_alloc_block();
        if (!phys) return 0;

        paging_map(kernel_page_directory, heap_current_limit, (uint32_t)phys, 3);
        heap_current_limit += PAGE_SIZE;
    }
    return 1;
}

void heap_init(void) {
    spinlock_init(&heap_lock);
    
    if (!heap_expand(PAGE_SIZE)) {
        vga_print("Failed to initialize kernel heap!\n");
        return;
    }

    head = (heap_header_t*)HEAP_START_ADDR;
    head->size = PAGE_SIZE - sizeof(heap_header_t);
    head->is_free = 1;
    head->next = 0;
}

void* kmalloc(size_t size) {
    if (size == 0) return 0;
    
    uint32_t flags = spinlock_acquire_safe(&heap_lock);

    size = (size + 3) & ~3;
    heap_header_t* curr = head;
    heap_header_t* last = 0;

    while (curr) {
        if (curr->is_free && curr->size >= size) {
            if (curr->size > size + sizeof(heap_header_t) + 4) {
                heap_header_t* split = (heap_header_t*)((uint8_t*)curr + sizeof(heap_header_t) + size);
                split->is_free = 1;
                split->size = curr->size - size - sizeof(heap_header_t);
                split->next = curr->next;
                
                curr->size = size;
                curr->next = split;
            }
            curr->is_free = 0;
            spinlock_release_safe(&heap_lock, flags);
            return (void*)((uint8_t*)curr + sizeof(heap_header_t));
        }
        last = curr;
        curr = curr->next;
    }

    uint32_t old_limit = heap_current_limit;
    if (!heap_expand(size)) {
        spinlock_release_safe(&heap_lock, flags);
        vga_set_color(COLOR_WHITE, COLOR_RED);
        vga_print("\n[KERNEL PANIC] Out Of Memory (Heap Expansion Failed)!\n");
        __asm__ volatile("cli; hlt");
        return 0;
    }

    heap_header_t* new_block = (heap_header_t*)old_limit;
    new_block->size = (heap_current_limit - old_limit) - sizeof(heap_header_t);
    new_block->is_free = 1;
    new_block->next = 0;

    if (last) last->next = new_block;

    new_block->is_free = 0;
    if (new_block->size > size + sizeof(heap_header_t) + 4) {
        heap_header_t* split = (heap_header_t*)((uint8_t*)new_block + sizeof(heap_header_t) + size);
        split->is_free = 1;
        split->size = new_block->size - size - sizeof(heap_header_t);
        split->next = 0;
        new_block->size = size;
        new_block->next = split;
    }

    spinlock_release_safe(&heap_lock, flags);
    return (void*)((uint8_t*)new_block + sizeof(heap_header_t));
}

void kfree(void* ptr) {
    if (!ptr) return;

    uint32_t addr = (uint32_t)ptr;
    void* ptr_to_free = ptr;

    if ((addr & 0xFFF) == 0) {
        uint32_t raw_ptr = *((uint32_t*)(addr - 4));
        if (raw_ptr >= HEAP_START_ADDR && raw_ptr < heap_current_limit) {
            ptr_to_free = (void*)raw_ptr;
        }
    }

    uint32_t flags = spinlock_acquire_safe(&heap_lock);
    heap_header_t* header = (heap_header_t*)((uint8_t*)ptr_to_free - sizeof(heap_header_t));
    header->is_free = 1;

    heap_header_t* curr = head;
    while (curr && curr->next) {
        if (curr->is_free && curr->next->is_free) {
            curr->size += curr->next->size + sizeof(heap_header_t);
            curr->next = curr->next->next;
        } else {
            curr = curr->next;
        }
    }
    spinlock_release_safe(&heap_lock, flags);
}

void* kmalloc_a(size_t n) {
    uint32_t raw_addr = (uint32_t)kmalloc(n + 4096 + 16);
    if (!raw_addr) return 0;
    uint32_t aligned_addr = (raw_addr + 4096) & ~0xFFF;
    *((uint32_t*)(aligned_addr - 4)) = raw_addr;
    return (void*)aligned_addr;
}