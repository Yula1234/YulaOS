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
    page_t* full;

    KmemCache* next_dyn;
};

constexpr size_t k_malloc_min_size = 8;
constexpr size_t k_malloc_max_size = 2048;

constexpr int k_malloc_shift_low = 3;
constexpr int k_malloc_shift_high = 11;

constexpr size_t k_cache_count = k_malloc_shift_high - k_malloc_shift_low + 1;

static_assert(k_malloc_shift_low >= 1, "k_malloc_shift_low must be at least 1");

constexpr size_t k_dynamic_cache_capacity = 32;

static constexpr uint32_t k_align_default = 0u;

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
            c->align = k_align_default;
            c->flags = 0;
            c->cpu_slab = nullptr;
            c->partial = nullptr;
            c->full = nullptr;

            size <<= 1;
        }

        init_dynamic_caches();
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

                    if (!page->freelist) {
                        cache.cpu_slab = nullptr;
                        slab_list_add(cache.full, *page);
                    }

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

                    if (!page->freelist) {
                        cache.cpu_slab = nullptr;
                        slab_list_add(cache.full, *page);
                    }

                    return obj;
                }
            }

            void* new_virt = vmm_->alloc_pages(1);
            if (kernel::unlikely(!new_virt)) {
                return nullptr;
            }

            const uint32_t phys = paging_get_phys(
                kernel_page_directory,
                static_cast<uint32_t>(reinterpret_cast<uintptr_t>(new_virt))
            );
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

        const uint32_t phys = paging_get_phys(
            kernel_page_directory,
            static_cast<uint32_t>(virt)
        );
        page_t* page = pmm_->phys_to_page(phys);

        if (kernel::unlikely(!page)) {
            panic("SLUB: free on invalid page");
        }

        if (kernel::unlikely(page->slab_cache != &cache)) {
            panic("SLUB: free cache mismatch");
        }

        const uintptr_t page_virt = virt & ~static_cast<uintptr_t>(PAGE_SIZE - 1);
        const uintptr_t off = virt - page_virt;

        if (kernel::unlikely(
                off >= PAGE_SIZE
                || cache.object_size == 0
                || (off % cache.object_size) != 0u
            )) {
            panic("SLUB: invalid object address");
        }

        bool need_free_page = false;

        {
            kernel::SpinLockSafeGuard guard(cache.lock);

            const bool was_full = (page->freelist == nullptr);
            const bool will_free_page = (page != cache.cpu_slab && page->objects == 1u);

            if (!will_free_page) {
                *reinterpret_cast<uintptr_t*>(obj) = reinterpret_cast<uintptr_t>(page->freelist) | 1u;
                page->freelist = obj;
            }

            page->objects--;

            if (was_full && page != cache.cpu_slab) {
                slab_list_remove(cache.full, *page);
                if (!will_free_page) {
                    slab_list_add(cache.partial, *page);
                }
            }

            if (will_free_page) {
                if (!was_full) {
                    slab_list_remove(cache.partial, *page);
                }

                page->slab_cache = nullptr;
                page->freelist = nullptr;
                page->objects = 0;
                page->prev = nullptr;
                page->next = nullptr;

                need_free_page = true;
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

        const uint32_t phys = paging_get_phys(
            kernel_page_directory,
            static_cast<uint32_t>(reinterpret_cast<uintptr_t>(ptr))
        );
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

        if (try_free_aligned(ptr)) {
            return;
        }

        const uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);

        if (kernel::unlikely(!heap_range_contains(addr))) {
            return;
        }

        const uint32_t phys = paging_get_phys(
            kernel_page_directory,
            static_cast<uint32_t>(addr)
        );
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

        if (!ptr) {
            return nullptr;
        }

        const size_t zero_size = get_allocated_size(ptr, size);

        memset(ptr, 0, zero_size);

        return ptr;
    }

    [[nodiscard]] void* malloc_aligned(size_t size, uint32_t align) noexcept {
        if (align == 0u) {
            return malloc(size);
        }

        if ((align & (align - 1u)) != 0u) {
            return nullptr;
        }

        if (align > PAGE_SIZE) {
            return nullptr;
        }

        if (align == PAGE_SIZE) {
            return malloc_a(size);
        }

        return malloc_aligned_small(size, align);
    }

    [[nodiscard]] void* malloc_a(size_t size) noexcept {
        if (kernel::unlikely(size == 0u)) {
            return nullptr;
        }

        const uint32_t pages_needed = static_cast<uint32_t>((size + PAGE_SIZE - 1u) / PAGE_SIZE);

        void* ptr = vmm_->alloc_pages(pages_needed);
        if (kernel::unlikely(!ptr)) {
            return nullptr;
        }

        const uint32_t phys = paging_get_phys(
            kernel_page_directory,
            static_cast<uint32_t>(reinterpret_cast<uintptr_t>(ptr))
        );

        page_t* p = pmm_->phys_to_page(phys);

        if (kernel::likely(p)) {
            p->slab_cache = nullptr;
            p->objects = pages_needed;
        }

        return ptr;
    }

    [[nodiscard]] void* realloc(void* ptr, size_t new_size) noexcept {
        if (kernel::unlikely(!ptr)) {
            return malloc(new_size);
        }

        if (kernel::unlikely(new_size == 0)) {
            free(ptr);

            return nullptr;
        }

        const uint32_t phys = paging_get_phys(
            kernel_page_directory,
            static_cast<uint32_t>(reinterpret_cast<uintptr_t>(ptr))
        );
        page_t* page = pmm_->phys_to_page(phys);

        if (kernel::unlikely(!page)) {
            return nullptr;
        }

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

    KmemCache* cache_create(const char* name, size_t size, uint32_t align, uint32_t flags) noexcept {
        if (kernel::unlikely(!name || size == 0u)) {
            return nullptr;
        }

        if (align != 0u && (align & (align - 1u)) != 0u) {
            return nullptr;
        }

        if (align != 0u && align > PAGE_SIZE) {
            return nullptr;
        }

        if (size < sizeof(uintptr_t)) {
            size = sizeof(uintptr_t);
        }

        if (align != 0u && (size % align) != 0u) {
            const size_t rem = size % align;
            size += align - rem;
        }

        if (kernel::unlikely(size > PAGE_SIZE)) {
            return nullptr;
        }

        kernel::SpinLockSafeGuard guard(dynamic_caches_lock_);

        KmemCache* cache = cache_create_locked(name, size, align, flags);
        if (!cache) {
            return nullptr;
        }

        cache->next_dyn = dynamic_used_head_;
        dynamic_used_head_ = cache;

        return cache;
    }

    int cache_destroy(KmemCache& cache) noexcept {
        uintptr_t free_page_virt = 0u;

        {
            kernel::SpinLockSafeGuard dyn_guard(dynamic_caches_lock_);

            if (!is_dynamic_cache(&cache)) {
                return 0;
            }

            {
                kernel::SpinLockSafeGuard cache_guard(cache.lock);

                if (cache.full || cache.partial) {
                    return 0;
                }

                if (cache.cpu_slab) {
                    page_t* page = cache.cpu_slab;

                    if (page->objects != 0u) {
                        return 0;
                    }

                    if (!page->freelist) {
                        return 0;
                    }

                    free_page_virt = reinterpret_cast<uintptr_t>(page->freelist)
                        & ~static_cast<uintptr_t>(PAGE_SIZE - 1u);

                    cache.cpu_slab = nullptr;

                    page->slab_cache = nullptr;
                    page->freelist = nullptr;
                    page->objects = 0;
                    page->prev = nullptr;
                    page->next = nullptr;
                }
            }

            remove_dynamic_cache_locked(cache);

            cache.next_dyn = dynamic_free_head_;
            dynamic_free_head_ = &cache;
        }

        if (free_page_virt != 0u) {
            vmm_->free_pages(reinterpret_cast<void*>(free_page_virt), 1);
        }

        return 1;
    }

private:
    struct AlignedAllocHeader {
        uint32_t magic;
        uint32_t align;
        uintptr_t original;
    };

    static constexpr uint32_t k_aligned_alloc_magic = 0x41A11CEDu;

    static uintptr_t align_up_uintptr(uintptr_t value, uintptr_t align) noexcept {
        return (value + (align - 1u)) & ~(align - 1u);
    }

    void* malloc_aligned_small(size_t size, uint32_t align) noexcept {
        const size_t header_size = sizeof(AlignedAllocHeader);

        if (kernel::unlikely(size > SIZE_MAX - header_size - align)) {
            return nullptr;
        }

        const size_t total = size + header_size + align;

        void* raw = malloc(total);
        if (kernel::unlikely(!raw)) {
            return nullptr;
        }

        const uintptr_t raw_addr = reinterpret_cast<uintptr_t>(raw);

        const uintptr_t aligned_addr = align_up_uintptr(
            raw_addr + header_size,
            align
        );
        auto* header = reinterpret_cast<AlignedAllocHeader*>(aligned_addr - header_size);

        header->magic = k_aligned_alloc_magic;
        header->align = align;
        header->original = raw_addr;

        return reinterpret_cast<void*>(aligned_addr);
    }

    bool try_free_aligned(void* ptr) noexcept {
        const uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);
        if (kernel::unlikely(addr < sizeof(AlignedAllocHeader))) {
            return false;
        }

        if (kernel::unlikely((addr & static_cast<uintptr_t>(PAGE_SIZE - 1u)) == 0u)) {
            return false;
        }

        const uintptr_t header_addr = addr - sizeof(AlignedAllocHeader);
        if (kernel::unlikely(!heap_range_contains(header_addr))) {
            return false;
        }

        const uint32_t header_phys = paging_get_phys(
            kernel_page_directory,
            static_cast<uint32_t>(header_addr)
        );
        if (kernel::unlikely(!header_phys)) {
            return false;
        }

        auto* header = reinterpret_cast<AlignedAllocHeader*>(header_addr);
        if (kernel::unlikely(header->magic != k_aligned_alloc_magic)) {
            return false;
        }

        const uint32_t align = header->align;
        if (kernel::unlikely(
                align == 0u
                || (align & (align - 1u)) != 0u
                || align > PAGE_SIZE
            )) {
            return false;
        }

        if (kernel::unlikely((addr & (static_cast<uintptr_t>(align) - 1u)) != 0u)) {
            return false;
        }

        const uintptr_t original_addr = header->original;
        if (kernel::unlikely(!heap_range_contains(original_addr))) {
            return false;
        }

        const uintptr_t min_aligned = original_addr + sizeof(AlignedAllocHeader);
        const uintptr_t max_aligned = min_aligned + static_cast<uintptr_t>(align);

        if (kernel::unlikely(addr < min_aligned || addr >= max_aligned)) {
            return false;
        }

        header->magic = 0u;
        header->align = 0u;

        free(reinterpret_cast<void*>(original_addr));
        return true;
    }

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
        if (kernel::unlikely(cache.object_size == 0u || cache.object_size > PAGE_SIZE)) {
            panic("SLUB: invalid object_size in slub_init_page");
        }

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

        *reinterpret_cast<uintptr_t*>(obj) = 0;

        return obj;
    }

    static bool heap_range_contains(uintptr_t addr) noexcept {
        const uint64_t start = static_cast<uint64_t>(KERNEL_HEAP_START);
        const uint64_t end = start + static_cast<uint64_t>(KERNEL_HEAP_SIZE);
        const uint64_t v = static_cast<uint64_t>(addr);

        return v >= start && v < end;
    }

    static int get_cache_index(size_t size) noexcept {
        if (size <= k_malloc_min_size) {
            return 0;
        }

        uint32_t msb_index;

        const uint32_t v = static_cast<uint32_t>(size - 1u);
        if (kernel::unlikely(v == 0u)) {
            return 0;
        }

        __asm__ volatile("bsr %1, %0"
                         : "=r"(msb_index)
                         : "r"(v));

        const int idx = static_cast<int>(msb_index) - (k_malloc_shift_low - 1);
        if (idx < 0) {
            return 0;
        }

        if (static_cast<size_t>(idx) >= k_cache_count) {
            return static_cast<int>(k_cache_count - 1u);
        }

        return idx;
    }

    static void copy_cache_name(KmemCache& cache, const char* name) noexcept {
        size_t i = 0;
        for (; i + 1u < sizeof(cache.name) && name[i] != '\0'; i++) {
            cache.name[i] = name[i];
        }

        cache.name[i] = '\0';
    }

    void init_dynamic_caches() noexcept {
        for (size_t i = 0; i < k_dynamic_cache_capacity; i++) {
            dynamic_caches_[i].next_dyn = dynamic_free_head_;
            dynamic_free_head_ = &dynamic_caches_[i];
        }

        dynamic_used_head_ = nullptr;
    }

    static size_t round_up_size(size_t size, size_t align) noexcept {
        if (size < sizeof(uintptr_t)) {
            size = sizeof(uintptr_t);
        }

        if (align == 0u) {
            return size;
        }

        const size_t rem = size % align;
        if (rem == 0u) {
            return size;
        }

        return size + (align - rem);
    }

    KmemCache* find_or_create_dynamic_cache(size_t object_size, uint32_t align, uint32_t flags) noexcept {
        kernel::SpinLockSafeGuard guard(dynamic_caches_lock_);

        for (KmemCache* it = dynamic_used_head_; it; it = it->next_dyn) {
            if (it->object_size != object_size) {
                continue;
            }

            if (it->align != align) {
                continue;
            }

            if (it->flags != flags) {
                continue;
            }

            return it;
        }

        KmemCache* created = cache_create_locked("dyn", object_size, align, flags);
        if (kernel::unlikely(!created)) {
            return nullptr;
        }

        created->next_dyn = dynamic_used_head_;
        dynamic_used_head_ = created;

        return created;
    }

    KmemCache* cache_create_locked(const char* name, size_t size, uint32_t align, uint32_t flags) noexcept {
        if (kernel::unlikely(!dynamic_free_head_)) {
            return nullptr;
        }

        KmemCache* cache = dynamic_free_head_;
        dynamic_free_head_ = dynamic_free_head_->next_dyn;

        memset(cache, 0, sizeof(*cache));

        copy_cache_name(*cache, name);

        cache->object_size = size;
        cache->align = align;
        cache->flags = flags;
        cache->cpu_slab = nullptr;
        cache->partial = nullptr;
        cache->full = nullptr;
        cache->next_dyn = nullptr;

        return cache;
    }

    bool is_dynamic_cache(const KmemCache* cache) const noexcept {
        const uintptr_t begin = reinterpret_cast<uintptr_t>(&dynamic_caches_[0]);
        const uintptr_t end = reinterpret_cast<uintptr_t>(&dynamic_caches_[k_dynamic_cache_capacity]);
        const uintptr_t p = reinterpret_cast<uintptr_t>(cache);

        return p >= begin && p < end;
    }

    void remove_dynamic_cache_locked(KmemCache& cache) noexcept {
        KmemCache** link = &dynamic_used_head_;

        while (*link) {
            if (*link == &cache) {
                *link = cache.next_dyn;
                cache.next_dyn = nullptr;
                return;
            }

            link = &(*link)->next_dyn;
        }
    }

    size_t get_allocated_size(void* ptr, size_t requested_size) const noexcept {
        const uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);

        if (kernel::unlikely(!heap_range_contains(addr))) {
            return requested_size;
        }

        const uint32_t phys = paging_get_phys(
            kernel_page_directory,
            static_cast<uint32_t>(addr)
        );
        if (kernel::unlikely(!phys)) {
            return requested_size;
        }

        page_t* page = pmm_->phys_to_page(phys);
        if (kernel::unlikely(!page)) {
            return requested_size;
        }

        if (page->slab_cache) {
            const auto* cache = static_cast<const KmemCache*>(page->slab_cache);
            if (kernel::likely(cache->object_size != 0u)) {
                return cache->object_size;
            }
        }

        if (page->objects != 0u) {
            return static_cast<size_t>(page->objects) * PAGE_SIZE;
        }

        return requested_size;
    }

    KmemCache caches_[k_cache_count]{};

    kernel::SpinLock dynamic_caches_lock_;
    KmemCache dynamic_caches_[k_dynamic_cache_capacity]{};
    KmemCache* dynamic_free_head_ = nullptr;
    KmemCache* dynamic_used_head_ = nullptr;

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

} // namespace

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

    return reinterpret_cast<kmem_cache_t*>(
        heap->cache_create(
            name,
            size,
            align,
            flags
        )
    );
}

int kmem_cache_destroy(kmem_cache_t* cache) {
    if (!cache) {
        return 0;
    }

    HeapState* heap = heap_state_if_inited();

    if (kernel::unlikely(!heap)) {
        return 0;
    }

    return heap->cache_destroy(*reinterpret_cast<KmemCache*>(cache));
}

}
