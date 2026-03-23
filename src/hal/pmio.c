// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 Yula1234

#include <hal/pmio.h>

#include <hal/io.h>
#include <hal/lock.h>

#include <mm/heap.h>

#include <lib/string.h>
#include <lib/rbtree.h>

enum {
    PMIO_REGION_MAGIC = 0x504D494Fu,
};

struct pmio_region {
    uint32_t magic;
    struct rb_node rb_node;
    uint16_t start;
    uint16_t end;
    uint16_t max_end;
    char name[32];
};

static struct rb_root g_pmio_regions;
static percpu_rwspinlock_t g_pmio_lock;

void pmio_init(void) {
    g_pmio_regions = RB_ROOT;

    percpu_rwspinlock_init(&g_pmio_lock);
}

static int pmio_is_overlap(uint16_t start1, uint16_t end1, uint16_t start2, uint16_t end2) {
    if (start1 > end2) {
        return 0;
    }

    if (start2 > end1) {
        return 0;
    }

    return 1;
}

static uint16_t pmio_max_u16(uint16_t a, uint16_t b) {
    return a > b ? a : b;
}

static uint16_t pmio_region_max_end(const pmio_region_t* region) {
    uint16_t max_end = region->end;

    if (region->rb_node.rb_left) {
        const pmio_region_t* left = rb_entry(region->rb_node.rb_left, pmio_region_t, rb_node);
        max_end = pmio_max_u16(max_end, left->max_end);
    }

    if (region->rb_node.rb_right) {
        const pmio_region_t* right = rb_entry(region->rb_node.rb_right, pmio_region_t, rb_node);
        max_end = pmio_max_u16(max_end, right->max_end);
    }

    return max_end;
}

static void pmio_augment_propagate(struct rb_node* node, struct rb_node* stop) {
    while (node != stop) {
        pmio_region_t* region = rb_entry(node, pmio_region_t, rb_node);
        region->max_end = pmio_region_max_end(region);

        node = rb_parent(node);
    }
}

static void pmio_augment_copy(struct rb_node* old, struct rb_node* new_node) {
    pmio_region_t* old_region = rb_entry(old, pmio_region_t, rb_node);
    pmio_region_t* new_region = rb_entry(new_node, pmio_region_t, rb_node);

    new_region->max_end = old_region->max_end;
}

static void pmio_augment_rotate(struct rb_node* old, struct rb_node* new_node) {
    pmio_region_t* old_region = rb_entry(old, pmio_region_t, rb_node);
    pmio_region_t* new_region = rb_entry(new_node, pmio_region_t, rb_node);

    old_region->max_end = pmio_region_max_end(old_region);
    new_region->max_end = pmio_region_max_end(new_region);
}

static const struct rb_augment_callbacks g_pmio_augment_callbacks = {
    .propagate = pmio_augment_propagate,
    .copy      = pmio_augment_copy,
    .rotate    = pmio_augment_rotate,
};

static pmio_region_t* pmio_find_conflict_locked(uint16_t start, uint16_t end) {
    struct rb_node* node = g_pmio_regions.rb_node;

    while (node) {
        pmio_region_t* region = rb_entry(node, pmio_region_t, rb_node);

        if (pmio_is_overlap(start, end, region->start, region->end)) {
            return region;
        }

        if (node->rb_left) {
            pmio_region_t* left = rb_entry(node->rb_left, pmio_region_t, rb_node);
            
            if (left->max_end >= start) {
                node = node->rb_left;
                continue;
            }
        }

        if (region->start > end) {
            break;
        }

        node = node->rb_right;
    }

    return 0;
}

static void pmio_insert_locked(pmio_region_t* new_region) {
    struct rb_node** link = &g_pmio_regions.rb_node;
    struct rb_node* parent = 0;

    while (*link) {
        pmio_region_t* region = rb_entry(*link, pmio_region_t, rb_node);
        parent = *link;

        if (region->max_end < new_region->max_end) {
            region->max_end = new_region->max_end;
        }

        if (new_region->start < region->start) {
            link = &(*link)->rb_left;
        } else if (new_region->start > region->start) {
            link = &(*link)->rb_right;
        } else if (new_region->end < region->end) {
            link = &(*link)->rb_left;
        } else {
            link = &(*link)->rb_right;
        }
    }

    rb_link_node(&new_region->rb_node, parent, link);
    rb_insert_color_augmented(&new_region->rb_node, &g_pmio_regions, &g_pmio_augment_callbacks);
}

static int pmio_is_linked_locked(const pmio_region_t* region) {
    if (!region) {
        return 0;
    }
    
    return rb_parent(&region->rb_node) != 0 || g_pmio_regions.rb_node == &region->rb_node;
}

pmio_region_t* pmio_find_conflict(uint16_t start, uint16_t count) {
    if (count == 0) {
        return 0;
    }

    const uint32_t end32 = (uint32_t)start + count - 1u;
    if (end32 > 0xFFFFu) {
        return 0;
    }

    const uint16_t end = (uint16_t)end32;

    percpu_rwspinlock_acquire_read(&g_pmio_lock);

    pmio_region_t* region = pmio_find_conflict_locked(start, end);
    
    percpu_rwspinlock_release_read(&g_pmio_lock);

    return region;
}

pmio_region_t* pmio_find_region(uint16_t port) {
    percpu_rwspinlock_acquire_read(&g_pmio_lock);

    pmio_region_t* region = pmio_find_conflict_locked(port, port);
    
    percpu_rwspinlock_release_read(&g_pmio_lock);

    return region;
}

pmio_region_t* pmio_request_region(uint16_t start, uint16_t count, const char* name) {
    if (count == 0) {
        return 0;
    }

    const uint32_t end32 = (uint32_t)start + count - 1u;

    if (end32 > 0xFFFFu) {
        return 0;
    }

    const uint16_t end = (uint16_t)end32;

    pmio_region_t* new_region = (pmio_region_t*)kmalloc(sizeof(pmio_region_t));
    if (!new_region) {
        return 0;
    }

    new_region->magic = PMIO_REGION_MAGIC;
    new_region->start = start;
    new_region->end = end;
    new_region->max_end = end;

    strlcpy(
        new_region->name,
        name ? name : "unknown",
        sizeof(new_region->name)
    );

    percpu_rwspinlock_acquire_write(&g_pmio_lock);

    if (pmio_find_conflict_locked(start, end)) {
        percpu_rwspinlock_release_write(&g_pmio_lock);
        kfree(new_region);
        
        return 0;
    }

    pmio_insert_locked(new_region);

    percpu_rwspinlock_release_write(&g_pmio_lock);

    return new_region;
}

void pmio_release_region(pmio_region_t* region) {
    if (!region) {
        return;
    }

    if (region->magic != PMIO_REGION_MAGIC) {
        return;
    }

    int removed = 0;
    percpu_rwspinlock_acquire_write(&g_pmio_lock);

    if (pmio_is_linked_locked(region)) {
        rb_erase_augmented(&region->rb_node, &g_pmio_regions, &g_pmio_augment_callbacks);

        region->rb_node.__parent_color = 0;
        region->rb_node.rb_left = 0;
        region->rb_node.rb_right = 0;

        removed = 1;
    }

    percpu_rwspinlock_release_write(&g_pmio_lock);

    if (removed) {
        region->magic = 0;
        kfree(region);
    }
}

uint8_t pmio_readb(uint16_t port) {
    return inb(port);
}

void pmio_writeb(uint16_t port, uint8_t val) {
    outb(port, val);
}

uint16_t pmio_readw(uint16_t port) {
    return inw(port);
}

void pmio_writew(uint16_t port, uint16_t val) {
    outw(port, val);
}

uint32_t pmio_readl(uint16_t port) {
    return inl(port);
}

void pmio_writel(uint16_t port, uint32_t val) {
    outl(port, val);
}