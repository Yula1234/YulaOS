/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2026 Yula1234 */

#include <lib/cpp/lock_guard.h>
#include <lib/cpp/atomic.h>
#include <lib/cpp/new.h>

#include <kernel/smp/cpu.h>
#include <kernel/panic.h>

#include <lib/compiler.h>
#include <lib/string.h>

#include <mm/pmm.h>
#include <mm/vmm.h>

#include <arch/i386/paging.h>

#include <hal/align.h>

extern "C" {

#include "heap.h"

}

namespace {

static constexpr uint32_t compute_reciprocal(uint32_t divisor) noexcept {
    if (divisor == 0u) {
        return 0u;
    }

    if (divisor == 1u) {
        return 0xFFFFFFFFu;
    }

    return static_cast<uint32_t>(((static_cast<uint64_t>(1) << 32) + divisor - 1) / divisor);
}

/*
 * Kernel heap implementation.
 *
 * This is a hybrid allocator:
 *  - small allocations come from size-segregated caches (SLUB-like)
 *  - large allocations are backed by whole pages from VMM
 *
 * Ownership and size tracking are stored in per-page metadata:
 *  - for slab objects: page->slab_cache points to the owning cache
 *  - for page-backed allocations: page->slab_cache is null and page->objects
 *    stores the page count
 */

struct RemoteFreeBatch {
    void* head = nullptr;
    void* tail = nullptr;

    uint32_t count = 0;
};

struct KmemCache {
    char name[16];

    size_t object_size;

    uint32_t reciprocal_size;

    uint32_t align;
    uint32_t flags;

    uint32_t pages_per_slab;

    kernel::SpinLock lock;

    struct __cacheline_aligned PerCpuSlab {
        page_t* page;

        kernel::atomic<void*> remote_free;
        kernel::atomic<uint32_t> remote_free_count;

        __cacheline_aligned RemoteFreeBatch remote_batches[MAX_CPUS];
    };

    PerCpuSlab cpu_slabs[MAX_CPUS];
    
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

static constexpr uint32_t k_align_default = 0u;

class HeapState {
public:
    void init() noexcept {
        /*
         * Heap sits on top of VMM (virtual range management) and PMM (page
         * metadata + backing page allocator).
         */
        vmm_ = kernel::vmm_state();
        pmm_ = kernel::pmm_state();

        size_t size = k_malloc_min_size;
        for (size_t i = 0; i < k_cache_count; i++) {
            KmemCache* c = &caches_[i];

            c->name[0] = 's';
            c->name[1] = '\0';

            c->object_size = size;
            c->reciprocal_size = compute_reciprocal(static_cast<uint32_t>(size));
            c->align = k_align_default;
            c->flags = 0;

            uint32_t pps = 1;
            
            if (size > 128 && size <= 512) {
                pps = 2;
            } else if (size > 512) {
                pps = 4;
            }

            c->pages_per_slab = pps;

            for (int cpu = 0; cpu < MAX_CPUS; cpu++) {
                c->cpu_slabs[cpu].page = nullptr;
                
                c->cpu_slabs[cpu].remote_free.store(nullptr);
                c->cpu_slabs[cpu].remote_free_count.store(0u);

                for (int tgt = 0; tgt < MAX_CPUS; tgt++) {
                    c->cpu_slabs[cpu].remote_batches[tgt].head = nullptr;
                    c->cpu_slabs[cpu].remote_batches[tgt].tail = nullptr;

                    c->cpu_slabs[cpu].remote_batches[tgt].count = 0;
                }
            }

            c->partial = nullptr;
            c->full = nullptr;

            size <<= 1;
        }
    }

private:
    static constexpr uint32_t k_owner_tag_mask = 0xFFu;
    static constexpr uint32_t k_remote_pending_shift = 8u;
    static constexpr uint32_t k_remote_pending_inc = 1u << k_remote_pending_shift;

    ___inline uint32_t page_owner_tag(const page_t& page) noexcept {
        const uint32_t v = __atomic_load_n(&page._reserved, __ATOMIC_ACQUIRE);

        return v & k_owner_tag_mask;
    }

    ___inline uint32_t page_remote_pending(const page_t& page) noexcept {
        const uint32_t v = __atomic_load_n(&page._reserved, __ATOMIC_ACQUIRE);

        return v >> k_remote_pending_shift;
    }

    ___inline void page_set_owner_tag(page_t& page, uint32_t tag) noexcept {
        const uint32_t tag_bits = tag & k_owner_tag_mask;

        uint32_t expected = __atomic_load_n(&page._reserved, __ATOMIC_RELAXED);
        while (true) {
            const uint32_t desired = (expected & ~k_owner_tag_mask) | tag_bits;

            if (__atomic_compare_exchange_n(
                    &page._reserved, &expected, desired,
                    false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED
                )) {
                return;
            }
        }
    }

    ___inline void page_remote_pending_inc(page_t& page) noexcept {
        __atomic_fetch_add(&page._reserved, k_remote_pending_inc, __ATOMIC_ACQ_REL);
    }

    ___inline void page_remote_pending_dec(page_t& page) noexcept {
        __atomic_fetch_sub(&page._reserved, k_remote_pending_inc, __ATOMIC_ACQ_REL);
    }

public:
    [[nodiscard]] void* cache_alloc(KmemCache& cache) noexcept {
        kernel::ScopedIrqDisable irq_guard;

        cpu_t* cpu = cpu_current();
        if (kernel::unlikely(!cpu)) {
            return nullptr;
        }

        const int cpu_index = cpu->index;
        if (kernel::unlikely(cpu_index < 0 || cpu_index >= MAX_CPUS)) {
            return nullptr;
        }

        KmemCache::PerCpuSlab& local = cache.cpu_slabs[cpu_index];

        page_t* page = local.page;

        if (kernel::likely(page != nullptr)) {
            
            if (kernel::unlikely(page->freelist == nullptr)) {
                drain_remote_frees(cache, local);
            }

            if (kernel::likely(page->freelist != nullptr)) {
                if (kernel::unlikely(page->slab_cache != &cache)) {
                    panic("SLUB: cpu slab cache mismatch");
                }

                void* obj = slub_alloc_from_page(*page);

                if (kernel::unlikely(page->freelist == nullptr)) {
                    finish_cpu_slab_full(cache, cpu_index, *page);
                }

                return obj;
            }
        }

        irq_guard.restore();

        return cache_alloc_slowpath(cache, cpu_index);
    }

    void cache_free(KmemCache& cache, void* obj) noexcept {
        if (kernel::unlikely(!obj)) {
            return;
        }

        kernel::ScopedIrqDisable irq_guard;

        cpu_t* cpu = cpu_current();
        if (kernel::unlikely(!cpu)) {
            return;
        }

        const int cpu_index = cpu->index;
        if (kernel::unlikely(cpu_index < 0 || cpu_index >= MAX_CPUS)) {
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

        uintptr_t page_virt = virt & ~static_cast<uintptr_t>(PAGE_SIZE - 1u);

        if (page->objects == 0xFFFFu) {
            page_virt = reinterpret_cast<uintptr_t>(page->list.prev);

            page = static_cast<page_t*>(page->freelist);
        }

        const uintptr_t off = virt - page_virt;

        if (kernel::unlikely(off >= static_cast<uintptr_t>(cache.pages_per_slab * PAGE_SIZE) || cache.object_size == 0)) {
            panic("SLUB: invalid object address");
        }

        const uint32_t index = static_cast<uint32_t>(
            (static_cast<uint64_t>(off) * cache.reciprocal_size) >> 32
        );

        const uintptr_t expected_off = static_cast<uintptr_t>(index) * cache.object_size;

        if (kernel::unlikely(off != expected_off)) {
            panic("SLUB: invalid object address");
        }

        const uint32_t owner_tag = page_owner_tag(*page);
        const int owner_cpu = static_cast<int>(owner_tag) - 1;

        if (owner_tag != 0u && owner_cpu >= 0 && owner_cpu < MAX_CPUS) {
            if (owner_cpu == cpu_index) {
                KmemCache::PerCpuSlab& local = cache.cpu_slabs[cpu_index];
                if (kernel::likely(local.page == page)) {
                    drain_remote_frees(cache, local);

                    if (kernel::unlikely(page->objects == 0u)) {
                        panic("SLUB: free objects underflow");
                    }

                    push_object_to_page_freelist(*page, obj);
                    page->objects--;
                    return;
                }
            } else {
                kernel::SpinLockSafeGuard guard(cache.lock);

                const uint32_t locked_owner_tag = page_owner_tag(*page);
                const int locked_owner_cpu = static_cast<int>(locked_owner_tag) - 1;

                if (locked_owner_tag != 0u && locked_owner_cpu == owner_cpu) {
                    if (kernel::likely(cache.cpu_slabs[owner_cpu].page == page)) {
                        remote_free_object_batched(
                            cache.cpu_slabs[cpu_index], cache.cpu_slabs[owner_cpu],
                            owner_cpu, *page, obj
                        );
                        return;
                    }
                }
            }
        }

        irq_guard.restore();
        cache_free_slowpath(cache, obj, *page, page_virt);
    }

    [[nodiscard]] void* malloc(size_t size) noexcept {
        if (kernel::unlikely(size == 0)) {
            return nullptr;
        }

        /*
         * Requests up to k_malloc_max_size use caches.
         * Above that we switch to page-backed allocations to avoid cache
         * blowups and excessive internal fragmentation.
         */
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

        /*
         * Aligned allocations for sub-page alignments carry a small header
         * in-band. Try to detect and free those first.
         */
        if (try_free_aligned(ptr)) {
            return;
        }

        const uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);

        /*
         * Heap API is only defined for addresses from the kernel heap range.
         * Foreign pointers are ignored.
         */
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

        if (page->slab_cache && page->objects == 0xFFFFu) {
            page = static_cast<page_t*>(page->freelist);
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
        uintptr_t free_pages_virt[MAX_CPUS]{};
        uint32_t free_pages_count = 0u;

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

                for (int cpu = 0; cpu < MAX_CPUS; cpu++) {
                    KmemCache::PerCpuSlab& local = cache.cpu_slabs[cpu];

                    if (local.remote_free.load() != nullptr) {
                        return 0;
                    }

                    if (local.remote_free_count.load() != 0u) {
                        return 0;
                    }

                    if (!local.page) {
                        continue;
                    }

                    page_t* page = local.page;

                    if (page->objects != 0u) {
                        return 0;
                    }

                    if (page_remote_pending(*page) != 0u) {
                        return 0;
                    }

                    if (!page->freelist) {
                        return 0;
                    }

                    if (free_pages_count >= MAX_CPUS) {
                        return 0;
                    }

                    uintptr_t page_virt = reinterpret_cast<uintptr_t>(page->freelist)
                        & ~static_cast<uintptr_t>(PAGE_SIZE - 1u);
                    
                    while (cache.pages_per_slab > 1) {
                        uint32_t p = paging_get_phys(kernel_page_directory, static_cast<uint32_t>(page_virt));
                        
                        if (pmm_->phys_to_page(p) == page) {
                            break;
                        }

                        page_virt -= PAGE_SIZE;
                    }

                    free_pages_virt[free_pages_count++] = page_virt;

                    local.page = nullptr;
                    page->slab_cache = nullptr;
                    page->freelist = nullptr;

                    page->objects = 0;
                    
                    page->list.prev = nullptr;
                    page->list.next = nullptr;
                    
                    page_set_owner_tag(*page, 0u);

                    for (uint32_t i = 1; i < cache.pages_per_slab; i++) {
                        uint32_t t_phys = paging_get_phys(kernel_page_directory, 
                                            static_cast<uint32_t>(page_virt + i * PAGE_SIZE));

                        page_t* t_page = pmm_->phys_to_page(t_phys);
                        
                        if (kernel::likely(t_page)) {
                            t_page->slab_cache = nullptr;
                            
                            t_page->objects = 0;
                            
                            t_page->freelist = nullptr;
                            t_page->list.prev = nullptr;
                        }
                    }
                }
            }

            remove_dynamic_cache_locked(cache);
        }

        for (uint32_t i = 0; i < free_pages_count; i++) {
            if (free_pages_virt[i] != 0u) {
                vmm_->free_pages(reinterpret_cast<void*>(free_pages_virt[i]), cache.pages_per_slab);
            }
        }

        free(&cache);

        return 1;
    }

private:
    struct AlignedAllocHeader {
        uint32_t magic;
        uint32_t align;
        uintptr_t original;
    };

    static constexpr uint32_t k_aligned_alloc_magic = 0x41A11CEDu;

    ___inline uintptr_t align_up_uintptr(uintptr_t value, uintptr_t align) noexcept {
        return (value + (align - 1u)) & ~(align - 1u);
    }

    void* malloc_aligned_small(size_t size, uint32_t align) noexcept {
        /*
         * Implement sub-page alignment via over-allocation and a small header
         * right before the aligned return address.
         */
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

        if (kernel::unlikely((addr & static_cast<uintptr_t>(PAGE_SIZE - 1u)) < sizeof(AlignedAllocHeader))) {
            if (kernel::unlikely(paging_get_phys(kernel_page_directory, static_cast<uint32_t>(header_addr)) == 0u)) {
                return false;
            }
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

        const uint32_t orig_phys = paging_get_phys(
            kernel_page_directory,
            static_cast<uint32_t>(original_addr)
        );

        if (kernel::unlikely(!orig_phys)) {
            return false;
        }

        page_t* orig_page = pmm_->phys_to_page(orig_phys);
        if (kernel::unlikely(!orig_page)) {
            return false;
        }

        if (orig_page->slab_cache) {
            uintptr_t base_virt = original_addr & ~static_cast<uintptr_t>(PAGE_SIZE - 1u);
            
            if (orig_page->objects == 0xFFFFu) {
                base_virt = reinterpret_cast<uintptr_t>(orig_page->list.prev);
                orig_page = static_cast<page_t*>(orig_page->freelist);
            }

            const auto* cache = static_cast<const KmemCache*>(orig_page->slab_cache);
            
            if (cache->object_size > 0u) {
                const uintptr_t offset_in_page = original_addr - base_virt;
            
                if (kernel::unlikely((offset_in_page % cache->object_size) != 0u)) {
                    return false;
                }
            }
        } else {
            if (kernel::unlikely((original_addr & static_cast<uintptr_t>(PAGE_SIZE - 1u)) != 0u)) {
                return false;
            }
        }

        header->magic = 0u;
        header->align = 0u;

        free(reinterpret_cast<void*>(original_addr));
        return true;
    }

    /*
     * Slab page lists are doubly-linked through page_t::prev/next.
     * cpu_slab is not part of these lists.
     */
    ___inline void slab_list_add(page_t*& head, page_t& page) noexcept {
        page.list.next = head;
        page.list.prev = nullptr;

        if (head) {
            head->list.prev = &page;
        }

        head = &page;
    }

    ___inline void slab_list_remove(page_t*& head, page_t& page) noexcept {
        if (page.list.prev) {
            page.list.prev->list.next = page.list.next;
        } else {
            head = page.list.next;
        }

        if (page.list.next) {
            page.list.next->list.prev = page.list.prev;
        }

        page.list.next = nullptr;
        page.list.prev = nullptr;
    }

    void slub_init_page(KmemCache& cache, page_t& page, void* virt_addr) noexcept {
        /*
         * Build a freelist inside the page.
         * Each free object stores the tagged pointer to the next free object.
         */
        if (kernel::unlikely(cache.object_size == 0u || cache.object_size > PAGE_SIZE)) {
            panic("SLUB: invalid object_size in slub_init_page");
        }

        page.slab_cache = &cache;
        page.objects = 0;

        page.list.next = nullptr;
        page.list.prev = nullptr;
        
        page_set_owner_tag(page, 0u);

        const uint32_t obj_count = (cache.pages_per_slab * PAGE_SIZE) / cache.object_size;
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
        /*
         * Freelist pointers are tagged with bit0 set.
         * This gives a cheap corruption detector since objects are naturally
         * aligned at least to sizeof(uintptr_t).
         */
        void* obj = page.freelist;

        if (!obj) {
            return nullptr;
        }

        const uintptr_t next_tagged = *reinterpret_cast<uintptr_t*>(obj);

        if (kernel::unlikely((next_tagged & 1u) == 0u)) {
            panic("SLUB: freelist tag corrupt");
        }

        void* next = reinterpret_cast<void*>(next_tagged & ~static_cast<uintptr_t>(1u));

        if (kernel::likely(next != nullptr)) {
            __builtin_prefetch(next, 1 /* write */, 0 /* temporal locality */);
        }

        page.freelist = next;
        page.objects++;

        *reinterpret_cast<uintptr_t*>(obj) = 0;

        return obj;
    }

    ___inline void push_object_to_page_freelist(page_t& page, void* obj) noexcept {
        if (kernel::unlikely(!obj)) {
            panic("SLUB: push null object");
        }

        *reinterpret_cast<uintptr_t*>(obj) = reinterpret_cast<uintptr_t>(page.freelist) | 1u;
        page.freelist = obj;
    }

    static void flush_remote_batch(KmemCache::PerCpuSlab& target, RemoteFreeBatch& batch) noexcept {
        if (kernel::unlikely(batch.count == 0)) {
            return;
        }

        void* current_head = target.remote_free.load(kernel::memory_order::relaxed);

        while (true) {
            *reinterpret_cast<uintptr_t*>(batch.tail) = reinterpret_cast<uintptr_t>(current_head) | 1u;

            if (target.remote_free.compare_exchange_weak(
                    current_head, batch.head,
                    kernel::memory_order::release,
                    kernel::memory_order::relaxed)) {
                target.remote_free_count.fetch_add(batch.count, kernel::memory_order::relaxed);
                break;
            }
        }

        batch.head = nullptr;
        batch.tail = nullptr;
        
        batch.count = 0;
    }

    static void remote_free_object_batched(
        KmemCache::PerCpuSlab& local_slab, KmemCache::PerCpuSlab& target_slab,
        int target_cpu, page_t& page, void* obj) noexcept
    {
        
        page_remote_pending_inc(page);

        RemoteFreeBatch& batch = local_slab.remote_batches[target_cpu];

        *reinterpret_cast<uintptr_t*>(obj) = reinterpret_cast<uintptr_t>(batch.head) | 1u;
        
        batch.head = obj;
        
        if (batch.count == 0) {
            batch.tail = obj;
        }
        
        batch.count++;

        if (batch.count >= 16) {
            flush_remote_batch(target_slab, batch);
        }
    }

    void drain_remote_frees(KmemCache& cache, KmemCache::PerCpuSlab& local) noexcept {
        if (kernel::likely(local.remote_free.load(kernel::memory_order::relaxed) == nullptr)) {
            return;
        }

        void* list = local.remote_free.exchange(nullptr, kernel::memory_order::acq_rel);
        const uint32_t count = local.remote_free_count.exchange(0u, kernel::memory_order::relaxed);

        if (kernel::unlikely(list == nullptr)) {

            if (kernel::unlikely(count != 0u)) {
                local.remote_free_count.fetch_add(count, kernel::memory_order::relaxed);
            }

            return;
        }

        uint32_t drained = 0u;
        void* it = list;
        while (it) {
            const uintptr_t next_tagged = *reinterpret_cast<uintptr_t*>(it);

            if (kernel::unlikely((next_tagged & 1u) == 0u)) {
                panic("SLUB: remote free tag corrupt");
            }

            void* next = reinterpret_cast<void*>(next_tagged & ~static_cast<uintptr_t>(1u));

            const uintptr_t virt = reinterpret_cast<uintptr_t>(it);
            const uint32_t phys = paging_get_phys(
                kernel_page_directory,
                static_cast<uint32_t>(virt)
            );

            page_t* page = pmm_->phys_to_page(phys);
            if (kernel::unlikely(!page)) {
                panic("SLUB: remote free on invalid page");
            }

            if (kernel::unlikely(page->slab_cache != &cache)) {
                panic("SLUB: remote free slab cache mismatch");
            }

            if (kernel::unlikely(page->objects == 0u)) {
                panic("SLUB: remote free objects underflow");
            }

            uintptr_t page_virt = virt & ~static_cast<uintptr_t>(PAGE_SIZE - 1u);
            
            if (page->objects == 0xFFFFu) {
                page_virt = reinterpret_cast<uintptr_t>(page->list.prev);
            
                page = static_cast<page_t*>(page->freelist);
            }

            if (kernel::likely(local.page == page)) {
                push_object_to_page_freelist(*page, it);
                page->objects--;
            } else {
                cache_free_slowpath(cache, it, *page, page_virt);
            }

            page_remote_pending_dec(*page);

            drained++;
            it = next;
        }

        if (drained < count) {
            local.remote_free_count.fetch_add(count - drained);
        }
    }

    void finish_cpu_slab_full(KmemCache& cache, int cpu_index, page_t& page) noexcept {
        kernel::SpinLockSafeGuard guard(cache.lock);

        KmemCache::PerCpuSlab& local = cache.cpu_slabs[cpu_index];
        if (local.page != &page) {
            return;
        }

        if (page.freelist != nullptr) {
            return;
        }

        local.page = nullptr;
        page_set_owner_tag(page, 0u);

        slab_list_add(cache.full, page);
    }

    [[nodiscard]] void* cache_alloc_slowpath(KmemCache& cache, int cpu_index) noexcept {
        while (true) {
            {
                kernel::SpinLockSafeGuard guard(cache.lock);

                KmemCache::PerCpuSlab& local = cache.cpu_slabs[cpu_index];

                if (local.page != nullptr) {
                    if (kernel::unlikely(local.page->slab_cache != &cache)) {
                        panic("SLUB: cpu slab cache mismatch in slowpath");
                    }

                    if (local.page->freelist == nullptr) {
                        page_t* full_page = local.page;
                        local.page = nullptr;

                        page_set_owner_tag(*full_page, 0u);
                        slab_list_add(cache.full, *full_page);
                    }
                }

                if (local.page == nullptr && cache.partial != nullptr) {
                    page_t* page = cache.partial;

                    if (kernel::unlikely(page->slab_cache != &cache)) {
                        panic("SLUB: partial page cache mismatch");
                    }

                    if (kernel::unlikely(page->freelist == nullptr)) {
                        panic("SLUB: partial page has null freelist");
                    }

                    slab_list_remove(cache.partial, *page);

                    local.page = page;
                    page_set_owner_tag(*page, static_cast<uint32_t>(cpu_index + 1));
                }
            }

            KmemCache::PerCpuSlab& local = cache.cpu_slabs[cpu_index];
            if (local.page != nullptr) {
                drain_remote_frees(cache, local);

                if (kernel::likely(local.page->freelist != nullptr)) {
                    void* obj = slub_alloc_from_page(*local.page);
                    if (kernel::unlikely(!obj)) {
                        continue;
                    }

                    if (kernel::unlikely(local.page->freelist == nullptr)) {
                        finish_cpu_slab_full(cache, cpu_index, *local.page);
                    }

                    return obj;
                }
            }

            void* new_virt = vmm_->alloc_pages(cache.pages_per_slab);
            if (kernel::unlikely(!new_virt)) {
                return nullptr;
            }

            const uint32_t phys = paging_get_phys(
                kernel_page_directory,
                static_cast<uint32_t>(reinterpret_cast<uintptr_t>(new_virt))
            );

            page_t* new_page = pmm_->phys_to_page(phys);

            if (kernel::unlikely(!new_page)) {
                vmm_->free_pages(new_virt, cache.pages_per_slab);
                return nullptr;
            }

            for (uint32_t i = 1; i < cache.pages_per_slab; i++) {
                const uint32_t tail_phys = paging_get_phys(
                    kernel_page_directory, 
                    static_cast<uint32_t>(reinterpret_cast<uintptr_t>(new_virt) + i * PAGE_SIZE)
                );

                page_t* tail_page = pmm_->phys_to_page(tail_phys);
                
                if (kernel::likely(tail_page)) {
                    tail_page->slab_cache = &cache;

                    tail_page->objects = 0xFFFFu;
                    tail_page->freelist = new_page;
                    
                    tail_page->list.prev = reinterpret_cast<page_t*>(new_virt);
                }
            }

            slub_init_page(cache, *new_page, new_virt);

            {
                kernel::SpinLockSafeGuard guard(cache.lock);

                KmemCache::PerCpuSlab& local = cache.cpu_slabs[cpu_index];
                if (local.page == nullptr) {
                    local.page = new_page;
                    page_set_owner_tag(*new_page, static_cast<uint32_t>(cpu_index + 1));
                } else {
                    page_set_owner_tag(*new_page, 0u);
                    slab_list_add(cache.partial, *new_page);
                }
            }
        }
    }

    void cache_free_slowpath(
        KmemCache& cache,
        void* obj,
        page_t& page,
        uintptr_t page_virt
    ) noexcept {
        bool need_free_page = false;

        {
            kernel::SpinLockSafeGuard guard(cache.lock);

            if (kernel::unlikely(page.objects == 0u)) {
                panic("SLUB: free objects underflow");
            }

            const uint32_t owner_tag = page_owner_tag(page);
            if (kernel::unlikely(owner_tag != 0u)) {
                const int owner_cpu = static_cast<int>(owner_tag) - 1;

                if (owner_cpu >= 0 && owner_cpu < MAX_CPUS) {
                    if (kernel::likely(cache.cpu_slabs[owner_cpu].page == &page)) {
                        const int local_cpu = cpu_current()->index;
                        
                        remote_free_object_batched(
                            cache.cpu_slabs[local_cpu], cache.cpu_slabs[owner_cpu],
                            owner_cpu, page, obj
                        );
                        return;
                    }
                }
            }

            const bool was_full = (page.freelist == nullptr);
            const bool will_free_page = (
                page_owner_tag(page) == 0u
                && page.objects == 1u
                && page_remote_pending(page) == 0u
            );

            if (!will_free_page) {
                push_object_to_page_freelist(page, obj);
            }

            page.objects--;

            if (page_owner_tag(page) == 0u) {
                if (was_full) {
                    slab_list_remove(cache.full, page);
                    if (!will_free_page) {
                        slab_list_add(cache.partial, page);
                    }
                }
            }

            if (will_free_page) {
                if (!was_full) {
                    slab_list_remove(cache.partial, page);
                }

                for (int cpu = 0; cpu < MAX_CPUS; cpu++) {
                    if (cache.cpu_slabs[cpu].page == &page) {
                        cache.cpu_slabs[cpu].page = nullptr;
                    }
                }

                page.slab_cache = nullptr;
                page.freelist = nullptr;
                
                page.objects = 0;
                
                page.list.prev = nullptr;
                page.list.next = nullptr;

                page_set_owner_tag(page, 0u);

                for (uint32_t i = 1; i < cache.pages_per_slab; i++) {
                    uint32_t t_phys = paging_get_phys(kernel_page_directory, 
                                        static_cast<uint32_t>(page_virt + i * PAGE_SIZE));

                    page_t* t_page = pmm_->phys_to_page(t_phys);
                    
                    if (kernel::likely(t_page)) {
                        t_page->slab_cache = nullptr;
                        
                        t_page->objects = 0;
                        
                        t_page->freelist = nullptr;
                        t_page->list.prev = nullptr;
                    }
                }

                need_free_page = true;
            }
        }

        if (need_free_page) {
            vmm_->free_pages(reinterpret_cast<void*>(page_virt), cache.pages_per_slab);
        }
    }

    ___inline bool heap_range_contains(uintptr_t addr) noexcept {
        const uint64_t start = static_cast<uint64_t>(KERNEL_HEAP_START);
        const uint64_t end = start + static_cast<uint64_t>(KERNEL_HEAP_SIZE);
        const uint64_t v = static_cast<uint64_t>(addr);

        return v >= start && v < end;
    }

    ___inline int get_cache_index(size_t size) noexcept {
        /*
         * Round size up to the next power-of-two bucket.
         * Implemented via bsr on (size - 1).
         */
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

    ___inline void copy_cache_name(KmemCache& cache, const char* name) noexcept {
        size_t i = 0;
        for (; i + 1u < sizeof(cache.name) && name[i] != '\0'; i++) {
            cache.name[i] = name[i];
        }

        cache.name[i] = '\0';
    }

    ___inline size_t round_up_size(size_t size, size_t align) noexcept {
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
        /* Caller is expected to hold dynamic_caches_lock_. */

        KmemCache* cache = static_cast<KmemCache*>(zalloc(sizeof(KmemCache)));
        if (kernel::unlikely(!cache)) {
            return nullptr;
        }

        copy_cache_name(*cache, name);

        cache->object_size = size;
        cache->reciprocal_size = compute_reciprocal(static_cast<uint32_t>(size));
        cache->align = align;
        cache->flags = flags;

        uint32_t pps = 1;
        
        if (size > 128 && size <= 512) {
            pps = 2;
        } else if (size > 512) {
            pps = 4;
        }
        
        cache->pages_per_slab = pps;

        cache->next_dyn = nullptr;

        return cache;
    }

    ___always_inline inline bool is_dynamic_cache(const KmemCache* cache) const noexcept {
        const uintptr_t begin = reinterpret_cast<uintptr_t>(&caches_[0]);
        const uintptr_t end = reinterpret_cast<uintptr_t>(&caches_[k_cache_count]);
        const uintptr_t p = reinterpret_cast<uintptr_t>(cache);

        return (p < begin || p >= end);
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
        /*
         * Used by kzalloc() to determine how much memory can safely be zeroed.
         * For slab objects we return cache->object_size.
         * For page-backed allocations we return page_count * PAGE_SIZE.
         */
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

        if (page->slab_cache && page->objects == 0xFFFFu) {
            page = static_cast<page_t*>(page->freelist);
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
    KmemCache* dynamic_used_head_ = nullptr;

    kernel::VmmState* vmm_ = nullptr;
    kernel::PmmState* pmm_ = nullptr;
};

alignas(HeapState) static unsigned char g_heap_storage[sizeof(HeapState)];
static HeapState* g_heap = nullptr;

___inline HeapState& heap_state_init_once() noexcept {
    if (!g_heap) {
        g_heap = new (g_heap_storage) HeapState();
    }

    return *g_heap;
}

___inline HeapState* heap_state_if_inited() noexcept {
    return g_heap;
}

} /* namespace */

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
