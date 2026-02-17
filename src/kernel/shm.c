// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 Yula1234

#include <kernel/shm.h>

#include <fs/vfs.h>

#include <hal/lock.h>

#include <mm/heap.h>
#include <mm/pmm.h>

#include <arch/i386/paging.h>

#include <lib/string.h>

typedef struct {
    spinlock_t lock;

    uint32_t size;
    uint32_t page_count;
    uint32_t* pages;

    volatile uint32_t refcount;
} shm_obj_t;

#define SHM_NAMED_MAX 64

typedef struct {
    int in_use;
    char name[32];
    shm_obj_t* obj;
} shm_named_entry_t;

static spinlock_t shm_named_lock;
static volatile uint32_t shm_named_init_done;
static shm_named_entry_t shm_named[SHM_NAMED_MAX];

static void shm_named_init_once(void) {
    uint32_t st = __atomic_load_n(&shm_named_init_done, __ATOMIC_ACQUIRE);
    if (st == 2u) return;

    if (st == 0u) {
        uint32_t expected = 0u;
        if (__atomic_compare_exchange_n(&shm_named_init_done, &expected, 1u, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            spinlock_init(&shm_named_lock);
            memset(shm_named, 0, sizeof(shm_named));
            __atomic_store_n(&shm_named_init_done, 2u, __ATOMIC_RELEASE);
            return;
        }
    }

    while (__atomic_load_n(&shm_named_init_done, __ATOMIC_ACQUIRE) != 2u) {
        __asm__ volatile("pause");
    }
}

static uint32_t shm_name_len_bounded(const char* name) {
    if (!name) return 0;
    for (uint32_t i = 0; i < 31u; i++) {
        if (name[i] == '\0') return i;
    }
    return 0;
}

static shm_obj_t* shm_obj_create(uint32_t size) {
    if (size == 0) return 0;

    uint32_t page_count = (size + 4095u) / 4096u;
    if (page_count == 0) return 0;

    shm_obj_t* obj = (shm_obj_t*)kmalloc(sizeof(shm_obj_t));
    if (!obj) return 0;
    memset(obj, 0, sizeof(*obj));
    spinlock_init(&obj->lock);

    obj->size = size;
    obj->page_count = page_count;
    obj->pages = (uint32_t*)kmalloc(sizeof(uint32_t) * page_count);
    if (!obj->pages) {
        kfree(obj);
        return 0;
    }
    memset(obj->pages, 0, sizeof(uint32_t) * page_count);

    for (uint32_t i = 0; i < page_count; i++) {
        void* p = pmm_alloc_block();
        if (!p) {
            for (uint32_t j = 0; j < i; j++) {
                if (obj->pages[j]) pmm_free_block((void*)obj->pages[j]);
            }
            kfree(obj->pages);
            kfree(obj);
            return 0;
        }

        paging_zero_phys_page((uint32_t)p);
        obj->pages[i] = (uint32_t)p;
    }

    obj->refcount = 1;
    return obj;
}

static void shm_obj_release(shm_obj_t* obj) {
    if (!obj) return;
    if (__sync_sub_and_fetch(&obj->refcount, 1u) != 0u) {
        return;
    }

    uint32_t flags = spinlock_acquire_safe(&obj->lock);
    uint32_t* pages = obj->pages;
    uint32_t page_count = obj->page_count;
    obj->pages = 0;
    obj->page_count = 0;
    spinlock_release_safe(&obj->lock, flags);

    if (pages) {
        for (uint32_t i = 0; i < page_count; i++) {
            if (pages[i]) pmm_free_block((void*)pages[i]);
        }
        kfree(pages);
    }
    kfree(obj);
}

static int shm_read(vfs_node_t* node, uint32_t offset, uint32_t size, void* buffer) {
    (void)node; (void)offset; (void)size; (void)buffer;
    return -1;
}

static int shm_write(vfs_node_t* node, uint32_t offset, uint32_t size, const void* buffer) {
    (void)node; (void)offset; (void)size; (void)buffer;
    return -1;
}

static int shm_close(vfs_node_t* node) {
    if (!node) return -1;

    shm_obj_t* obj = (shm_obj_t*)node->private_data;
    if (obj) shm_obj_release(obj);
    kfree(node);
    return 0;
}

static vfs_ops_t shm_ops = {
    .read = shm_read,
    .write = shm_write,
    .open = 0,
    .close = shm_close,
};

int shm_get_phys_pages(struct vfs_node* node, const uint32_t** out_pages, uint32_t* out_page_count) {
    if (!node || !out_pages || !out_page_count) return 0;

    *out_pages = 0;
    *out_page_count = 0;

    if ((node->flags & VFS_FLAG_SHM) == 0) return 0;

    shm_obj_t* obj = (shm_obj_t*)node->private_data;
    if (!obj) return 0;

    uint32_t flags = spinlock_acquire_safe(&obj->lock);
    const uint32_t* pages = obj->pages;
    uint32_t page_count = obj->page_count;
    spinlock_release_safe(&obj->lock, flags);

    if (!pages || page_count == 0) return 0;

    *out_pages = pages;
    *out_page_count = page_count;
    return 1;
}

struct vfs_node* shm_create_node(uint32_t size) {
    shm_obj_t* obj = shm_obj_create(size);
    if (!obj) return 0;

    vfs_node_t* node = (vfs_node_t*)kmalloc(sizeof(vfs_node_t));
    if (!node) {
        shm_obj_release(obj);
        return 0;
    }

    memset(node, 0, sizeof(*node));
    strlcpy(node->name, "shm", sizeof(node->name));
    node->flags = VFS_FLAG_SHM;
    node->size = size;
    node->inode_idx = 0;
    node->refs = 1;
    node->ops = &shm_ops;
    node->private_data = obj;

    return node;
}

struct vfs_node* shm_create_named_node(const char* name, uint32_t size) {
    shm_named_init_once();
    uint32_t nlen = shm_name_len_bounded(name);
    if (nlen == 0) return 0;
    if (name[nlen] != '\0') return 0;

    shm_obj_t* obj = shm_obj_create(size);
    if (!obj) return 0;

    shm_obj_t* to_release[SHM_NAMED_MAX];
    int rel_n = 0;

    uint32_t gl_flags = spinlock_acquire_safe(&shm_named_lock);
    for (int i = 0; i < SHM_NAMED_MAX; i++) {
        if (!shm_named[i].in_use) continue;
        if (shm_named[i].name[0] == name[0] && strcmp(shm_named[i].name, name) == 0) {
            spinlock_release_safe(&shm_named_lock, gl_flags);
            shm_obj_release(obj);
            return 0;
        }
    }

    int slot = -1;
    for (int i = 0; i < SHM_NAMED_MAX; i++) {
        if (!shm_named[i].in_use) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        for (int i = 0; i < SHM_NAMED_MAX; i++) {
            if (!shm_named[i].in_use) continue;
            shm_obj_t* o = shm_named[i].obj;
            if (!o) continue;

            uint32_t rc = __atomic_load_n(&o->refcount, __ATOMIC_RELAXED);
            if (rc == 1u) {
                shm_named[i].in_use = 0;
                shm_named[i].obj = 0;
                memset(shm_named[i].name, 0, sizeof(shm_named[i].name));
                to_release[rel_n++] = o;
            }
        }

        for (int i = 0; i < SHM_NAMED_MAX; i++) {
            if (!shm_named[i].in_use) {
                slot = i;
                break;
            }
        }
    }
    if (slot < 0) {
        spinlock_release_safe(&shm_named_lock, gl_flags);
        for (int i = 0; i < rel_n; i++) {
            shm_obj_release(to_release[i]);
        }
        shm_obj_release(obj);
        return 0;
    }

    shm_named[slot].in_use = 1;
    strlcpy(shm_named[slot].name, name, sizeof(shm_named[slot].name));
    shm_named[slot].obj = obj;
    __sync_fetch_and_add(&obj->refcount, 1u);
    spinlock_release_safe(&shm_named_lock, gl_flags);

    for (int i = 0; i < rel_n; i++) {
        shm_obj_release(to_release[i]);
    }

    vfs_node_t* node = (vfs_node_t*)kmalloc(sizeof(vfs_node_t));
    if (!node) {
        shm_unlink_named(name);
        shm_obj_release(obj);
        return 0;
    }

    memset(node, 0, sizeof(*node));
    strlcpy(node->name, "shm", sizeof(node->name));
    node->flags = VFS_FLAG_SHM;
    node->size = size;
    node->inode_idx = 0;
    node->refs = 1;
    node->ops = &shm_ops;
    node->private_data = obj;
    return node;
}

struct vfs_node* shm_open_named_node(const char* name) {
    shm_named_init_once();
    uint32_t nlen = shm_name_len_bounded(name);
    if (nlen == 0) return 0;
    if (name[nlen] != '\0') return 0;

    shm_obj_t* obj = 0;

    uint32_t gl_flags = spinlock_acquire_safe(&shm_named_lock);
    for (int i = 0; i < SHM_NAMED_MAX; i++) {
        if (!shm_named[i].in_use) continue;
        if (shm_named[i].name[0] == name[0] && strcmp(shm_named[i].name, name) == 0) {
            obj = shm_named[i].obj;
            break;
        }
    }
    if (obj) {
        __sync_fetch_and_add(&obj->refcount, 1u);
    }
    spinlock_release_safe(&shm_named_lock, gl_flags);

    if (!obj) return 0;

    vfs_node_t* node = (vfs_node_t*)kmalloc(sizeof(vfs_node_t));
    if (!node) {
        shm_obj_release(obj);
        return 0;
    }

    memset(node, 0, sizeof(*node));
    strlcpy(node->name, "shm", sizeof(node->name));
    node->flags = VFS_FLAG_SHM;
    node->size = obj->size;
    node->inode_idx = 0;
    node->refs = 1;
    node->ops = &shm_ops;
    node->private_data = obj;
    return node;
}

int shm_unlink_named(const char* name) {
    shm_named_init_once();
    uint32_t nlen = shm_name_len_bounded(name);
    if (nlen == 0) return -1;
    if (name[nlen] != '\0') return -1;

    shm_obj_t* obj = 0;

    uint32_t gl_flags = spinlock_acquire_safe(&shm_named_lock);
    for (int i = 0; i < SHM_NAMED_MAX; i++) {
        if (!shm_named[i].in_use) continue;
        if (shm_named[i].name[0] == name[0] && strcmp(shm_named[i].name, name) == 0) {
            obj = shm_named[i].obj;
            shm_named[i].in_use = 0;
            shm_named[i].obj = 0;
            memset(shm_named[i].name, 0, sizeof(shm_named[i].name));
            break;
        }
    }
    spinlock_release_safe(&shm_named_lock, gl_flags);

    if (!obj) return -1;
    shm_obj_release(obj);
    return 0;
}
