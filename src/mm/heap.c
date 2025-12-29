#include <arch/i386/paging.h>
#include <lib/string.h>
#include <hal/lock.h>
#include <drivers/vga.h>
#include <mm/pmm.h>

#include "heap.h"
#include "vmm.h"

#define KMALLOC_MIN_SIZE 8
#define KMALLOC_MAX_SIZE 2048

struct kmem_cache {
    char name[16];
    size_t object_size;
    uint32_t align;
    uint32_t flags;
    spinlock_t lock;
    page_t* cpu_slab;
    page_t* partial;
};

#define KMALLOC_SHIFT_LOW  3
#define KMALLOC_SHIFT_HIGH 11
static kmem_cache_t kmalloc_caches[KMALLOC_SHIFT_HIGH - KMALLOC_SHIFT_LOW + 1];

static void slab_list_add(page_t** head, page_t* page) {
    page->next = *head;
    page->prev = NULL;
    if (*head) {
        (*head)->prev = page;
    }
    *head = page;
}

static void slab_list_remove(page_t** head, page_t* page) {
    if (page->prev) {
        page->prev->next = page->next;
    } else {
        *head = page->next;
    }
    if (page->next) {
        page->next->prev = page->prev;
    }
    page->next = NULL;
    page->prev = NULL;
}

static int slub_init_page(kmem_cache_t* cache, page_t* page, void* virt_addr) {
    page->slab_cache = cache;
    page->objects = 0;
    
    page->next = NULL;
    page->prev = NULL;
    
    uint32_t obj_count = PAGE_SIZE / cache->object_size;
    uint8_t* base = (uint8_t*)virt_addr;
    void** prev_link = NULL;
    
    for (uint32_t i = 0; i < obj_count; i++) {
        void** current_obj = (void**)(base + i * cache->object_size);
        if (i == 0) page->freelist = current_obj;
        else *prev_link = current_obj;
        prev_link = current_obj;
    }
    if (prev_link) *prev_link = NULL;
    return 1;
}

static void* slub_alloc_from_page(page_t* page) {
    void* obj = page->freelist;
    if (!obj) return 0;
    
    page->freelist = *(void**)obj;
    page->objects++;
    
    *(uint32_t*)obj = 0; 
    return obj;
}

void* kmem_cache_alloc(kmem_cache_t* cache) {
    uint32_t flags = spinlock_acquire_safe(&cache->lock);
    
    page_t* page = cache->cpu_slab;
    
    if (page && page->freelist) {
        void* obj = slub_alloc_from_page(page);
        spinlock_release_safe(&cache->lock, flags);
        return obj;
    }
    
    if (cache->partial) {
        page = cache->partial;
        slab_list_remove(&cache->partial, page);
        cache->cpu_slab = page;
        void* obj = slub_alloc_from_page(page);
        spinlock_release_safe(&cache->lock, flags);
        return obj;
    }

    void* new_virt = vmm_alloc_pages(1);
    if (!new_virt) {
        spinlock_release_safe(&cache->lock, flags);
        return 0;
    }
    
    uint32_t phys = paging_get_phys(kernel_page_directory, (uint32_t)new_virt);
    page = pmm_phys_to_page(phys);
    
    slub_init_page(cache, page, new_virt);
    
    cache->cpu_slab = page;
    
    void* obj = slub_alloc_from_page(page);
    spinlock_release_safe(&cache->lock, flags);
    return obj;
}

void kmem_cache_free(kmem_cache_t* cache, void* obj) {
    if (!obj) return;
    uint32_t flags = spinlock_acquire_safe(&cache->lock);
    
    uint32_t virt = (uint32_t)obj;
    uint32_t phys = paging_get_phys(kernel_page_directory, virt);
    page_t* page = pmm_phys_to_page(phys);
    
    if (!page || page->slab_cache != cache) {
        spinlock_release_safe(&cache->lock, flags);
        return;
    }
    
    *(void**)obj = page->freelist;
    page->freelist = obj;
    page->objects--;
    
    if (page == cache->cpu_slab) {

    } else {
        uint32_t max_objs = PAGE_SIZE / cache->object_size;
        
        if (page->objects == 0) {
            slab_list_remove(&cache->partial, page);
            
            uint32_t page_virt = virt & ~0xFFF;

            spinlock_release_safe(&cache->lock, flags);
            
            vmm_free_pages((void*)page_virt, 1);
            return;
            
        } else if (page->objects == max_objs - 1) {
            slab_list_add(&cache->partial, page);
        }
    }
    
    spinlock_release_safe(&cache->lock, flags);
}

static inline int get_cache_index(size_t size) {
    if (size <= 8) return 0;

    volatile uint32_t msb_index;

    __asm__ volatile("bsr %1, %0" : "=r"(msb_index) : "r"(size - 1));

    return (int)(msb_index - 2);
}

void heap_init(void) {
    vmm_init();

    size_t size = 8;
    for (int i = 0; i <= (KMALLOC_SHIFT_HIGH - KMALLOC_SHIFT_LOW); i++) {
        kmem_cache_t* c = &kmalloc_caches[i];
        c->name[0] = 's'; c->name[1] = 0;
        c->object_size = size;
        c->align = 0;
        c->flags = 0;
        c->cpu_slab = 0;
        c->partial = 0;
        spinlock_init(&c->lock);
        size <<= 1;
    }
}

void* kmalloc(size_t size) {
    if (size == 0) return 0;
    if (size <= KMALLOC_MAX_SIZE) {
        int idx = get_cache_index(size);
        return kmem_cache_alloc(&kmalloc_caches[idx]);
    }
    
    uint32_t pages_needed = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    
    void* ptr = vmm_alloc_pages(pages_needed);
    if (!ptr) return 0;

    uint32_t phys = paging_get_phys(kernel_page_directory, (uint32_t)ptr);
    page_t* p = pmm_phys_to_page(phys);
    if (p) {
        p->slab_cache = NULL; 
        p->objects = pages_needed; 
    }
    
    return ptr;
}

void kfree(void* ptr) {
    if (!ptr) return;
    uint32_t addr = (uint32_t)ptr;
    
    if (addr < KERNEL_HEAP_START || addr >= KERNEL_HEAP_START + KERNEL_HEAP_SIZE) return;
    
    uint32_t phys = paging_get_phys(kernel_page_directory, addr);
    if (!phys) return;
    
    page_t* page = pmm_phys_to_page(phys);
    if (!page) return;
    
    if (page->slab_cache) {
        kmem_cache_free((kmem_cache_t*)page->slab_cache, ptr);
    } else {
        uint32_t pages_count = page->objects;
        if (pages_count > 0) {
            vmm_free_pages(ptr, pages_count);
            page->objects = 0;
        }
    }
}

void* kzalloc(size_t size) {
    void* ptr = kmalloc(size);
    if (ptr) memset(ptr, 0, size);
    return ptr;
}

void* kmalloc_aligned(size_t size, uint32_t align) {
    if (align >= 4096) {
        if (size < align) size = align;
        return kmalloc(size);
    }
    if (align > size) return kmalloc(align);
    return kmalloc(size);
}

void* kmalloc_a(size_t size) {
    return kmalloc_aligned(size, 4096);
}

void* krealloc(void* ptr, size_t new_size) {
    if (!ptr) return kmalloc(new_size);
    if (new_size == 0) { kfree(ptr); return 0; }
    
    uint32_t phys = paging_get_phys(kernel_page_directory, (uint32_t)ptr);
    page_t* page = pmm_phys_to_page(phys);
    
    size_t old_size;
    if (page->slab_cache) {
        old_size = ((kmem_cache_t*)page->slab_cache)->object_size;
    } else {
        old_size = page->objects * PAGE_SIZE;
    }
    
    if (new_size <= old_size) return ptr;
    
    void* new_ptr = kmalloc(new_size);
    if (new_ptr) {
        memcpy(new_ptr, ptr, old_size);
        kfree(ptr);
    }
    return new_ptr;
}