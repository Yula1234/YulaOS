// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 Yula1234

#include <hal/mmio.h>

#include <arch/i386/paging.h>
#include <hal/lock.h>

#include <mm/heap.h>

#include <lib/string.h>
#include <lib/rbtree.h>

enum {
    MMIO_REGION_MAGIC = 0x4D4D494Fu, // 'MMIO'
};

typedef struct {
    uint32_t magic_;
    struct rb_node rb_node_;

    uint32_t phys_start_;
    uint32_t phys_end_;
    uint32_t max_end_;

    uint32_t vaddr_;
    uint32_t size_;

    char name_[32];
} MmioRegion;

static struct rb_root g_mmio_regions;
static percpu_rwspinlock_t g_mmio_lock;

void mmio_init(void) {
    g_mmio_regions = RB_ROOT;
    percpu_rwspinlock_init(&g_mmio_lock);
}

static int mmio_is_overlap(uint32_t start1, uint32_t end1, uint32_t start2, uint32_t end2) {
    if (start1 > end2) {
        return 0;
    }

    if (start2 > end1) {
        return 0;
    }

    return 1;
}

static uint32_t mmio_max_u32(uint32_t a, uint32_t b) {
    return (a > b) ? a : b;
}

static uint32_t mmio_region_max_end(const MmioRegion* region) {
    uint32_t max_end = region->phys_end_;

    if (region->rb_node_.rb_left) {
        const MmioRegion* left = rb_entry(region->rb_node_.rb_left, MmioRegion, rb_node_);
        max_end = mmio_max_u32(max_end, left->max_end_);
    }

    if (region->rb_node_.rb_right) {
        const MmioRegion* right = rb_entry(region->rb_node_.rb_right, MmioRegion, rb_node_);
        max_end = mmio_max_u32(max_end, right->max_end_);
    }

    return max_end;
}

static void mmio_augment_propagate(struct rb_node* node, struct rb_node* stop) {
    while (node != stop) {
        MmioRegion* region = rb_entry(node, MmioRegion, rb_node_);
        region->max_end_ = mmio_region_max_end(region);

        node = rb_parent(node);
    }
}

static void mmio_augment_copy(struct rb_node* old, struct rb_node* new_node) {
    MmioRegion* old_region = rb_entry(old, MmioRegion, rb_node_);
    MmioRegion* new_region = rb_entry(new_node, MmioRegion, rb_node_);

    new_region->max_end_ = old_region->max_end_;
}

static void mmio_augment_rotate(struct rb_node* old, struct rb_node* new_node) {
    MmioRegion* old_region = rb_entry(old, MmioRegion, rb_node_);
    MmioRegion* new_region = rb_entry(new_node, MmioRegion, rb_node_);

    old_region->max_end_ = mmio_region_max_end(old_region);
    new_region->max_end_ = mmio_region_max_end(new_region);
}

static const struct rb_augment_callbacks g_mmio_augment_callbacks = {
    .propagate = mmio_augment_propagate,
    .copy      = mmio_augment_copy,
    .rotate    = mmio_augment_rotate,
};

static MmioRegion* mmio_find_conflict_locked(uint32_t start, uint32_t end) {
    struct rb_node* node = g_mmio_regions.rb_node;

    while (node) {
        MmioRegion* region = rb_entry(node, MmioRegion, rb_node_);

        if (mmio_is_overlap(start, end, region->phys_start_, region->phys_end_)) {
            return region;
        }

        if (node->rb_left) {
            MmioRegion* left = rb_entry(node->rb_left, MmioRegion, rb_node_);

            if (left->max_end_ >= start) {
                node = node->rb_left;
                continue;
            }
        }

        if (region->phys_start_ > end) {
            break;
        }

        node = node->rb_right;
    }

    return 0;
}

static void mmio_insert_locked(MmioRegion* new_region) {
    struct rb_node** link = &g_mmio_regions.rb_node;
    struct rb_node* parent = 0;

    while (*link) {
        MmioRegion* region = rb_entry(*link, MmioRegion, rb_node_);
        parent = *link;

        if (region->max_end_ < new_region->max_end_) {
            region->max_end_ = new_region->max_end_;
        }

        if (new_region->phys_start_ < region->phys_start_) {
            link = &(*link)->rb_left;
        } else if (new_region->phys_start_ > region->phys_start_) {
            link = &(*link)->rb_right;
        } else if (new_region->phys_end_ < region->phys_end_) {
            link = &(*link)->rb_left;
        } else {
            link = &(*link)->rb_right;
        }
    }

    rb_link_node(&new_region->rb_node_, parent, link);
    rb_insert_color_augmented(&new_region->rb_node_, &g_mmio_regions, &g_mmio_augment_callbacks);
}

static int mmio_map_pages(uint32_t phys_start, uint32_t size, uint32_t flags) {
    const uint32_t start_page = phys_start & ~0xFFFu;
    const uint32_t end_page = (phys_start + size + 0xFFFu) & ~0xFFFu;

    if (end_page < start_page) {
        return 0;
    }

    for (uint32_t p = start_page; p < end_page; p += 0x1000u) {
        /*
         * Identity mapping into kernel directory.
         * For 32-bit systems, devices usually reside in the 3GB-4GB range.
         */
        paging_map(kernel_page_directory, p, p, flags);

        if (p + 0x1000u < p) {
            break;
        }
    }

    return 1;
}

static void mmio_unmap_pages(uint32_t phys_start, uint32_t size) {
    const uint32_t start_page = phys_start & ~0xFFFu;
    const uint32_t end_page = (phys_start + size + 0xFFFu) & ~0xFFFu;

    if (end_page <= start_page) {
        return;
    }

    paging_unmap_range(kernel_page_directory, start_page, end_page);
}

static mmio_region_t* mmio_request_region_internal(uint32_t phys_start, uint32_t size, const char* name, uint32_t pte_flags) {
    if (size == 0u) {
        return 0;
    }

    const uint32_t phys_end = phys_start + size - 1u;

    if (phys_end < phys_start) {
        return 0; // Overflow
    }

    MmioRegion* new_region = (MmioRegion*)kmalloc(sizeof(MmioRegion));

    if (!new_region) {
        return 0;
    }

    new_region->magic_ = MMIO_REGION_MAGIC;
    new_region->phys_start_ = phys_start;
    new_region->phys_end_ = phys_end;
    new_region->max_end_ = phys_end;
    new_region->size_ = size;
    new_region->vaddr_ = phys_start; // Currently identity mapped

    strlcpy(
        new_region->name_,
        name ? name : "unknown",
        sizeof(new_region->name_)
    );

    percpu_rwspinlock_acquire_write(&g_mmio_lock);

    if (mmio_find_conflict_locked(phys_start, phys_end)) {
        goto out_unlock_fail;
    }

    if (!mmio_map_pages(phys_start, size, pte_flags)) {
        goto out_unlock_fail;
    }

    mmio_insert_locked(new_region);

    percpu_rwspinlock_release_write(&g_mmio_lock);

    return (mmio_region_t*)new_region;

out_unlock_fail:
    percpu_rwspinlock_release_write(&g_mmio_lock);
    kfree(new_region);
    return 0;
}

mmio_region_t* mmio_request_region(uint32_t phys_start, uint32_t size, const char* name) {
    /* 
     * PTE_PRESENT (1) | PTE_RW (2) | PTE_PCD (16) | PTE_PWT (8) = 0x1B
     * Strict Uncacheable (UC) mapping. Safe for command/status registers.
     */
    const uint32_t flags = PTE_PRESENT | PTE_RW | PTE_PCD | PTE_PWT;

    return mmio_request_region_internal(phys_start, size, name, flags);
}

mmio_region_t* mmio_request_region_wc(uint32_t phys_start, uint32_t size, const char* name) {
    /* 
     * PTE_PRESENT (1) | PTE_RW (2) = 0x3
     * If PAT is supported, we add PTE_PAT (0x80) to enable Write-Combining (WC).
     * If PAT is unsupported, it falls back to UC or WT depending on MTRR.
     */
    uint32_t flags = PTE_PRESENT | PTE_RW;

    if (paging_pat_is_supported()) {
        flags |= PTE_PAT;
    } else {
        flags |= PTE_PCD | PTE_PWT; // Fallback to safe UC
    }

    return mmio_request_region_internal(phys_start, size, name, flags);
}

void mmio_release_region(mmio_region_t* region_handle) {
    if (!region_handle) {
        return;
    }

    MmioRegion* region = (MmioRegion*)region_handle;

    if (region->magic_ != MMIO_REGION_MAGIC) {
        return;
    }

    int removed = 0;

    percpu_rwspinlock_acquire_write(&g_mmio_lock);

    /* Verify it's actually in the tree */
    if (rb_parent(&region->rb_node_) != 0 || g_mmio_regions.rb_node == &region->rb_node_) {
        rb_erase_augmented(&region->rb_node_, &g_mmio_regions, &g_mmio_augment_callbacks);

        region->rb_node_.__parent_color = 0;
        region->rb_node_.rb_left = 0;
        region->rb_node_.rb_right = 0;

        removed = 1;
    }

    percpu_rwspinlock_release_write(&g_mmio_lock);

    if (removed) {
        mmio_unmap_pages(region->phys_start_, region->size_);

        region->magic_ = 0;
        kfree(region);
    }
}

void* mmio_get_vaddr(mmio_region_t* region_handle) {
    if (!region_handle) {
        return 0;
    }

    MmioRegion* region = (MmioRegion*)region_handle;

    if (region->magic_ != MMIO_REGION_MAGIC) {
        return 0;
    }

    return (void*)(uintptr_t)region->vaddr_;
}

static int mmio_is_valid_access(const MmioRegion* region, uint32_t offset, uint32_t width) {
    if (offset + width > region->size_ || offset + width < offset) {
        return 0;
    }

    return 1;
}

int mmio_read8(mmio_region_t* region_handle, uint32_t offset, uint8_t* out_value) {
    if (!region_handle || !out_value) {
        return -1;
    }

    MmioRegion* region = (MmioRegion*)region_handle;

    if (region->magic_ != MMIO_REGION_MAGIC) {
        return -1;
    }

    if (!mmio_is_valid_access(region, offset, 1u)) {
        return -1;
    }

    volatile uint8_t* ptr = (volatile uint8_t*)(uintptr_t)(region->vaddr_ + offset);
    *out_value = *ptr;

    return 0;
}

int mmio_read16(mmio_region_t* region_handle, uint32_t offset, uint16_t* out_value) {
    if (!region_handle || !out_value) {
        return -1;
    }

    MmioRegion* region = (MmioRegion*)region_handle;

    if (region->magic_ != MMIO_REGION_MAGIC) {
        return -1;
    }

    if (!mmio_is_valid_access(region, offset, 2u)) {
        return -1;
    }

    volatile uint16_t* ptr = (volatile uint16_t*)(uintptr_t)(region->vaddr_ + offset);
    *out_value = *ptr;

    return 0;
}

int mmio_read32(mmio_region_t* region_handle, uint32_t offset, uint32_t* out_value) {
    if (!region_handle || !out_value) {
        return -1;
    }

    MmioRegion* region = (MmioRegion*)region_handle;

    if (region->magic_ != MMIO_REGION_MAGIC) {
        return -1;
    }

    if (!mmio_is_valid_access(region, offset, 4u)) {
        return -1;
    }

    volatile uint32_t* ptr = (volatile uint32_t*)(uintptr_t)(region->vaddr_ + offset);
    *out_value = *ptr;

    return 0;
}

int mmio_write8(mmio_region_t* region_handle, uint32_t offset, uint8_t value) {
    if (!region_handle) {
        return -1;
    }

    MmioRegion* region = (MmioRegion*)region_handle;

    if (region->magic_ != MMIO_REGION_MAGIC) {
        return -1;
    }

    if (!mmio_is_valid_access(region, offset, 1u)) {
        return -1;
    }

    volatile uint8_t* ptr = (volatile uint8_t*)(uintptr_t)(region->vaddr_ + offset);
    *ptr = value;

    return 0;
}

int mmio_write16(mmio_region_t* region_handle, uint32_t offset, uint16_t value) {
    if (!region_handle) {
        return -1;
    }

    MmioRegion* region = (MmioRegion*)region_handle;

    if (region->magic_ != MMIO_REGION_MAGIC) {
        return -1;
    }

    if (!mmio_is_valid_access(region, offset, 2u)) {
        return -1;
    }

    volatile uint16_t* ptr = (volatile uint16_t*)(uintptr_t)(region->vaddr_ + offset);
    *ptr = value;

    return 0;
}

int mmio_write32(mmio_region_t* region_handle, uint32_t offset, uint32_t value) {
    if (!region_handle) {
        return -1;
    }

    MmioRegion* region = (MmioRegion*)region_handle;

    if (region->magic_ != MMIO_REGION_MAGIC) {
        return -1;
    }

    if (!mmio_is_valid_access(region, offset, 4u)) {
        return -1;
    }

    volatile uint32_t* ptr = (volatile uint32_t*)(uintptr_t)(region->vaddr_ + offset);
    *ptr = value;

    return 0;
}