// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 Yula1234

#include <lib/compiler.h>

#include <lib/cpp/atomic.h>
#include <lib/cpp/lock_guard.h>
#include <lib/cpp/new.h>

#include <kernel/panic.h>

#include <lib/string.h>

#include <arch/i386/paging.h>

#include <mm/pmm.h>
#include <mm/vmm.h>

extern "C" {

#include "heap.h"

}

namespace {

struct KmemCache {
    char name[16];
    size_t object_size;
    uint32_t align;
    uint32_t flags;
    kernel::SpinLock lock;
    page_t* cpu_slab;
    page_t* partial;
};

constexpr size_t k_malloc_min_size = 8;
constexpr size_t k_malloc_max_size = 2048;
constexpr int k_malloc_shift_low = 3;
constexpr int k_malloc_shift_high = 11;
constexpr size_t k_cache_count = k_malloc_shift_high - k_malloc_shift_low + 1;

class HeapState {
public:
    void init() noexcept {
        vmm_ = kernel::vmm_state();
        pmm_ = kernel::pmm_state();

        size_t size = k_malloc_min_size;
        for (size_t i = 0; i < k_cache_count; i++) {
            KmemCache* c = &caches_[i];

            c->name[0] = 's';
            c->name[1] = '\0';

            c->object_size = size;
            c->align = 0;
            c->flags = 0;
            c->cpu_slab = nullptr;
            c->partial = nullptr;

            size <<= 1;
        }
    }

    [[nodiscard]] void* cache_alloc(KmemCache& cache) noexcept {
        while (true) {
            page_t* page = nullptr;
            void* obj = nullptr;

            {
                kernel::SpinLockSafeGuard guard(cache.lock);

                page = cache.cpu_slab;

                if (page && page->freelist) {
                    if (kernel::unlikely(page->slab_cache != &cache)) {
                        panic("SLUB: cpu_slab cache mismatch");
                    }

                    obj = slub_alloc_from_page(*page);
                    return obj;
                }

                if (cache.partial) {
                    page = cache.partial;

                    if (kernel::unlikely(page->slab_cache != &cache)) {
                        panic("SLUB: partial page cache mismatch");
                    }

                    if (kernel::unlikely(!page->freelist)) {
                        panic("SLUB: partial page has null freelist");
                    }

                    slab_list_remove(cache.partial, *page);
                    cache.cpu_slab = page;

                    obj = slub_alloc_from_page(*page);
                    return obj;
                }
            }

            void* new_virt = vmm_->alloc_pages(1);
            if (kernel::unlikely(!new_virt)) {
                return nullptr;
            }

            const uint32_t phys = paging_get_phys(kernel_page_directory, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(new_virt)));
            page_t* new_page = pmm_->phys_to_page(phys);

            if (kernel::unlikely(!new_page)) {
                vmm_->free_pages(new_virt, 1);
                return nullptr;
            }

            slub_init_page(cache, *new_page, new_virt);

            {
                kernel::SpinLockSafeGuard guard(cache.lock);

                if (!cache.cpu_slab) {
                    cache.cpu_slab = new_page;
                } else {
                    slab_list_add(cache.partial, *new_page);
                }
            }
        }
    }

    void cache_free(KmemCache& cache, void* obj) noexcept {
        if (kernel::unlikely(!obj)) {
            return;
        }

        const uintptr_t virt = reinterpret_cast<uintptr_t>(obj);
        const uint32_t phys = paging_get_phys(kernel_page_directory, static_cast<uint32_t>(virt));
        page_t* page = pmm_->phys_to_page(phys);

        if (kernel::unlikely(!page)) {
            panic("SLUB: free on invalid page");
        }

        if (kernel::unlikely(page->slab_cache != &cache)) {
            panic("SLUB: free cache mismatch");
        }

        const uintptr_t page_virt = virt & ~static_cast<uintptr_t>(PAGE_SIZE - 1);
        const uintptr_t off = virt - page_virt;

        if (kernel::unlikely(off >= PAGE_SIZE || cache.object_size == 0 || (off % cache.object_size) != 0u)) {
            panic("SLUB: invalid object address");
        }

        bool need_free_page = false;

        {
            kernel::SpinLockSafeGuard guard(cache.lock);

            *reinterpret_cast<uintptr_t*>(obj) = reinterpret_cast<uintptr_t>(page->freelist) | 1u;
            page->freelist = obj;
            page->objects--;

            if (page != cache.cpu_slab) {
                if (page->objects == 0) {
                    slab_list_remove(cache.partial, *page);

                    page->slab_cache = nullptr;
                    page->freelist = nullptr;
                    page->objects = 0;
                    page->prev = nullptr;
                    page->next = nullptr;

                    need_free_page = true;
                } else {
                    const uint32_t max_objs = PAGE_SIZE / cache.object_size;

                    if (page->objects == max_objs - 1) {
                        slab_list_add(cache.partial, *page);
                    }
                }
            }
        }

        if (need_free_page) {
            vmm_->free_pages(reinterpret_cast<void*>(page_virt), 1);
        }
    }

    [[nodiscard]] void* malloc(size_t size) noexcept {
        if (kernel::unlikely(size == 0)) {
            return nullptr;
        }

        if (kernel::likely(size <= k_malloc_max_size)) {
            const int idx = get_cache_index(size);
            return cache_alloc(caches_[idx]);
        }

        const uint32_t pages_needed = static_cast<uint32_t>((size + PAGE_SIZE - 1) / PAGE_SIZE);

        void* ptr = vmm_->alloc_pages(pages_needed);
        if (kernel::unlikely(!ptr)) {
            return nullptr;
        }

        const uint32_t phys = paging_get_phys(kernel_page_directory, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(ptr)));
        page_t* p = pmm_->phys_to_page(phys);

        if (kernel::likely(p)) {
            p->slab_cache = nullptr;
            p->objects = pages_needed;
        }

        return ptr;
    }

    void free(void* ptr) noexcept {
        if (kernel::unlikely(!ptr)) {
            return;
        }

        const uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);

        if (kernel::unlikely(!heap_range_contains(addr))) {
            return;
        }

        const uint32_t phys = paging_get_phys(kernel_page_directory, static_cast<uint32_t>(addr));
        if (kernel::unlikely(!phys)) {
            return;
        }

        page_t* page = pmm_->phys_to_page(phys);
        if (kernel::unlikely(!page)) {
            return;
        }

        if (kernel::likely(page->slab_cache)) {
            cache_free(*static_cast<KmemCache*>(page->slab_cache), ptr);
        } else {
            const uint32_t pages_count = page->objects;

            if (kernel::unlikely(pages_count == 0)) {
                panic("HEAP: kfree non-slab with zero pages");
            }

            vmm_->free_pages(ptr, pages_count);
            page->objects = 0;
        }
    }

    [[nodiscard]] void* zalloc(size_t size) noexcept {
        void* ptr = malloc(size);

        if (kernel::likely(ptr)) {
            memset(ptr, 0, size);
        }

        return ptr;
    }

    [[nodiscard]] void* malloc_aligned(size_t size, uint32_t align) noexcept {
        if (align >= PAGE_SIZE) {
            if (size < align) {
                size = align;
            }
            return malloc(size);
        }

        if (align > size) {
            return malloc(align);
        }

        return malloc(size);
    }

    [[nodiscard]] void* malloc_a(size_t size) noexcept {
        return malloc_aligned(size, PAGE_SIZE);
    }

    [[nodiscard]] void* realloc(void* ptr, size_t new_size) noexcept {
        if (kernel::unlikely(!ptr)) {
            return malloc(new_size);
        }

        if (kernel::unlikely(new_size == 0)) {
            free(ptr);
            return nullptr;
        }

        const uint32_t phys = paging_get_phys(kernel_page_directory, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(ptr)));
        page_t* page = pmm_phys_to_page(phys);

        size_t old_size;
        if (page->slab_cache) {
            old_size = static_cast<KmemCache*>(page->slab_cache)->object_size;
        } else {
            old_size = static_cast<size_t>(page->objects) * PAGE_SIZE;
        }

        if (kernel::likely(new_size <= old_size)) {
            return ptr;
        }

        void* new_ptr = malloc(new_size);
        if (kernel::likely(new_ptr)) {
            memcpy(new_ptr, ptr, old_size);
            free(ptr);
        }

        return new_ptr;
    }

    KmemCache* cache_create(const char*, size_t, uint32_t, uint32_t) noexcept {
        return nullptr;
    }

private:
    static void slab_list_add(page_t*& head, page_t& page) noexcept {
        page.next = head;
        page.prev = nullptr;

        if (head) {
            head->prev = &page;
        }

        head = &page;
    }

    static void slab_list_remove(page_t*& head, page_t& page) noexcept {
        if (page.prev) {
            page.prev->next = page.next;
        } else {
            head = page.next;
        }

        if (page.next) {
            page.next->prev = page.prev;
        }

        page.next = nullptr;
        page.prev = nullptr;
    }

    void slub_init_page(KmemCache& cache, page_t& page, void* virt_addr) noexcept {
        page.slab_cache = &cache;
        page.objects = 0;
        page.next = nullptr;
        page.prev = nullptr;

        const uint32_t obj_count = PAGE_SIZE / cache.object_size;
        uint8_t* base = static_cast<uint8_t*>(virt_addr);

        for (uint32_t i = 0; i < obj_count; i++) {
            uintptr_t* current_obj = reinterpret_cast<uintptr_t*>(base + i * cache.object_size);
            uintptr_t tagged = reinterpret_cast<uintptr_t>(base + (i + 1u) * cache.object_size) | 1u;

            if (i + 1u >= obj_count) {
                tagged = 1u;
            }

            if (i == 0) {
                page.freelist = current_obj;
            }

            *current_obj = tagged;
        }
    }

    void* slub_alloc_from_page(page_t& page) noexcept {
        void* obj = page.freelist;
        if (!obj) {
            return nullptr;
        }

        const uintptr_t next_tagged = *reinterpret_cast<uintptr_t*>(obj);

        if ((next_tagged & 1u) == 0u) {
            panic("SLUB: freelist tag corrupt");
        }

        void* next = reinterpret_cast<void*>(next_tagged & ~static_cast<uintptr_t>(1u));
        page.freelist = next;
        page.objects++;

        *reinterpret_cast<uint32_t*>(obj) = 0;
        return obj;
    }

    static bool heap_range_contains(uintptr_t addr) noexcept {
        const uint64_t start = static_cast<uint64_t>(KERNEL_HEAP_START);
        const uint64_t end = start + static_cast<uint64_t>(KERNEL_HEAP_SIZE);
        const uint64_t v = static_cast<uint64_t>(addr);

        return v >= start && v < end;
    }

    static int get_cache_index(size_t size) noexcept {
        if (size <= 8) {
            return 0;
        }

        uint32_t msb_index;

        __asm__ volatile("bsr %1, %0"
                         : "=r"(msb_index)
                         : "r"(static_cast<uint32_t>(size - 1)));

        return static_cast<int>(msb_index) - 2;
    }

    KmemCache caches_[k_cache_count]{};

    kernel::VmmState* vmm_ = nullptr;
    kernel::PmmState* pmm_ = nullptr;
};

alignas(HeapState) static unsigned char g_heap_storage[sizeof(HeapState)];
static HeapState* g_heap = nullptr;

static inline HeapState& heap_state_init_once() noexcept {
    if (!g_heap) {
        g_heap = new (g_heap_storage) HeapState();
    }

    return *g_heap;
}

static inline HeapState* heap_state_if_inited() noexcept {
    return g_heap;
}

}

extern "C" {

void heap_init(void) {
    HeapState& heap = heap_state_init_once();

    heap.init();
}

void* kmalloc(size_t size) {
    HeapState* heap = heap_state_if_inited();

    if (kernel::unlikely(!heap)) {
        return nullptr;
    }

    return heap->malloc(size);
}

void* kzalloc(size_t size) {
    HeapState* heap = heap_state_if_inited();

    if (kernel::unlikely(!heap)) {
        return nullptr;
    }

    return heap->zalloc(size);
}

void* krealloc(void* ptr, size_t new_size) {
    HeapState* heap = heap_state_if_inited();

    if (kernel::unlikely(!heap)) {
        return nullptr;
    }

    return heap->realloc(ptr, new_size);
}

void kfree(void* ptr) {
    HeapState* heap = heap_state_if_inited();

    if (kernel::unlikely(!heap)) {
        return;
    }

    heap->free(ptr);
}

void* kmalloc_aligned(size_t size, uint32_t align) {
    HeapState* heap = heap_state_if_inited();

    if (kernel::unlikely(!heap)) {
        return nullptr;
    }

    return heap->malloc_aligned(size, align);
}

void* kmalloc_a(size_t size) {
    HeapState* heap = heap_state_if_inited();

    if (kernel::unlikely(!heap)) {
        return nullptr;
    }

    return heap->malloc_a(size);
}

void* kmem_cache_alloc(kmem_cache_t* cache) {
    if (!cache) {
        return nullptr;
    }

    HeapState* heap = heap_state_if_inited();

    if (kernel::unlikely(!heap)) {
        return nullptr;
    }

    return heap->cache_alloc(*reinterpret_cast<KmemCache*>(cache));
}

void kmem_cache_free(kmem_cache_t* cache, void* obj) {
    if (!cache) {
        return;
    }

    HeapState* heap = heap_state_if_inited();

    if (kernel::unlikely(!heap)) {
        return;
    }

    heap->cache_free(*reinterpret_cast<KmemCache*>(cache), obj);
}

kmem_cache_t* kmem_cache_create(const char* name, size_t size, uint32_t align, uint32_t flags) {
    HeapState* heap = heap_state_if_inited();

    if (kernel::unlikely(!heap)) {
        return nullptr;
    }

    return reinterpret_cast<kmem_cache_t*>(heap->cache_create(name, size, align, flags));
}

}
