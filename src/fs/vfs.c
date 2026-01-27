// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#include "vfs.h"
#include "yulafs.h"
#include "../kernel/proc.h"
#include "../lib/string.h"
#include "../mm/heap.h"
#include <hal/lock.h>

typedef struct devfs_entry {
    vfs_node_t* node;
    struct devfs_entry* next;
} devfs_entry_t;

#define DEVFS_BUCKETS 256u

static devfs_entry_t* devfs_buckets[DEVFS_BUCKETS];
static spinlock_t devfs_locks[DEVFS_BUCKETS];
static volatile uint32_t devfs_init_done;

static uint32_t devfs_hash_name(const char* s) {
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; s[i] != '\0'; i++) {
        h ^= (uint8_t)s[i];
        h *= 16777619u;
    }
    return h;
}

static void devfs_init_once(void) {
    if (__sync_lock_test_and_set(&devfs_init_done, 1u) == 0u) {
        memset(devfs_buckets, 0, sizeof(devfs_buckets));
        for (uint32_t i = 0; i < DEVFS_BUCKETS; i++) {
            spinlock_init(&devfs_locks[i]);
        }
    }
}

void devfs_register(vfs_node_t* node) {
    if (!node || node->name[0] == '\0') return;
    devfs_init_once();

    uint32_t idx = devfs_hash_name(node->name) & (DEVFS_BUCKETS - 1u);
    uint32_t flags = spinlock_acquire_safe(&devfs_locks[idx]);

    for (devfs_entry_t* e = devfs_buckets[idx]; e; e = e->next) {
        vfs_node_t* n = e->node;
        if (n && n->name[0] == node->name[0] && strcmp(n->name, node->name) == 0) {
            e->node = node;
            spinlock_release_safe(&devfs_locks[idx], flags);
            return;
        }
    }

    devfs_entry_t* e = (devfs_entry_t*)kmalloc(sizeof(*e));
    if (!e) {
        spinlock_release_safe(&devfs_locks[idx], flags);
        return;
    }

    e->node = node;
    e->next = devfs_buckets[idx];
    devfs_buckets[idx] = e;

    spinlock_release_safe(&devfs_locks[idx], flags);
}

int devfs_unregister(const char* name) {
    if (!name || name[0] == '\0') return -1;
    devfs_init_once();

    uint32_t idx = devfs_hash_name(name) & (DEVFS_BUCKETS - 1u);
    uint32_t flags = spinlock_acquire_safe(&devfs_locks[idx]);

    devfs_entry_t* prev = 0;
    devfs_entry_t* cur = devfs_buckets[idx];
    while (cur) {
        vfs_node_t* n = cur->node;
        if (n && name[0] == n->name[0] && strcmp(n->name, name) == 0) {
            if (prev) prev->next = cur->next;
            else devfs_buckets[idx] = cur->next;
            spinlock_release_safe(&devfs_locks[idx], flags);
            kfree(cur);
            return 0;
        }
        prev = cur;
        cur = cur->next;
    }

    spinlock_release_safe(&devfs_locks[idx], flags);
    return -1;
}

vfs_node_t* devfs_fetch(const char* name) {
    if (!name || name[0] == '\0') return 0;
    devfs_init_once();

    uint32_t idx = devfs_hash_name(name) & (DEVFS_BUCKETS - 1u);
    uint32_t flags = spinlock_acquire_safe(&devfs_locks[idx]);

    for (devfs_entry_t* e = devfs_buckets[idx]; e; e = e->next) {
        vfs_node_t* n = e->node;
        if (n && name[0] == n->name[0] && strcmp(n->name, name) == 0) {
            spinlock_release_safe(&devfs_locks[idx], flags);
            return n;
        }
    }

    spinlock_release_safe(&devfs_locks[idx], flags);
    return 0;
}

int vfs_getdents(int fd, void* buf, uint32_t size) {
    task_t* curr = proc_current();
    file_t* f = proc_fd_get(curr, fd);
    if (!f || !f->used) return -1;
    if (!buf || size == 0) return -1;

    vfs_node_t* node = f->node;
    if (!node) return -1;
    if ((node->flags & VFS_FLAG_YULAFS) == 0) return -1;

    return yulafs_getdents(node->inode_idx, &f->offset, (yfs_dirent_info_t*)buf, size);
}

typedef struct {
    uint32_t type;
    uint32_t size;
} __attribute__((packed)) vfs_stat_t;

int vfs_fstatat(int dirfd, const char* name, void* stat_buf) {
    task_t* curr = proc_current();
    file_t* f = proc_fd_get(curr, dirfd);
    if (!f || !f->used) return -1;
    if (!name || !*name || !stat_buf) return -1;

    vfs_node_t* node = f->node;
    if (!node) return -1;
    if ((node->flags & VFS_FLAG_YULAFS) == 0) return -1;

    int ino = yulafs_lookup_in_dir(node->inode_idx, name);
    if (ino < 0) return -1;

    yfs_inode_t info;
    if (yulafs_stat((yfs_ino_t)ino, &info) != 0) return -1;

    vfs_stat_t* st = (vfs_stat_t*)stat_buf;
    st->type = info.type;
    st->size = info.size;
    return 0;
}

static int yfs_read_wrapper(vfs_node_t* node, uint32_t offset, uint32_t size, void* buffer) {
    return yulafs_read(node->inode_idx, buffer, offset, size);
}

static int yfs_write_wrapper(vfs_node_t* node, uint32_t offset, uint32_t size, const void* buffer) {
    return yulafs_write(node->inode_idx, buffer, offset, size);
}

static vfs_ops_t yfs_vfs_ops = {
    .read = yfs_read_wrapper,
    .write = yfs_write_wrapper,
    .open = 0, .close = 0
};

int vfs_open(const char* path, int flags) {
    task_t* curr = proc_current();
    if (!curr) return -1;

    vfs_node_t* node = 0;

    const char* target_path = path;
    
    if (path[0] == '/' && path[1] != '\0') {
        target_path++; 
    }
    
    if (strncmp(target_path, "dev/", 4) == 0) {
        vfs_node_t* dev = devfs_fetch(target_path + 4);
        if (dev) {
            node = (vfs_node_t*)kmalloc(sizeof(vfs_node_t));
            if (node) memcpy(node, dev, sizeof(vfs_node_t));
        }
    } 
    else {
        int inode = yulafs_lookup(path); 
        
        if (inode == -1 && flags == 1) {
            inode = yulafs_create(path);
        }

        if (inode != -1) {
            if (flags == 1) {
                yfs_inode_t info;
                yulafs_stat(inode, &info);
                
                if (info.type == YFS_TYPE_FILE) {
                    yulafs_resize(inode, 0);
                }
            }

            node = (vfs_node_t*)kmalloc(sizeof(vfs_node_t));
            if (node) {
                memset(node, 0, sizeof(vfs_node_t));
                node->inode_idx = inode;
                node->ops = &yfs_vfs_ops;
                node->flags = VFS_FLAG_YULAFS;
                
                yfs_inode_t info;
                if (yulafs_stat(inode, &info) == 0) {
                    node->size = info.size;
                }
                strlcpy(node->name, path, sizeof(node->name));
            }
        }
    }

    if (!node) return -1;

    node->refs = 1;

    if (node->ops && node->ops->open) {
        node->ops->open(node);
    }

    file_t* f = 0;
    int fd = proc_fd_alloc(curr, &f);
    if (fd < 0 || !f) {
        if (__sync_sub_and_fetch(&node->refs, 1) == 0) {
            if (node->ops && node->ops->close) node->ops->close(node);
            else kfree(node);
        }
        return -1;
    }

    f->node = node;
    f->offset = 0;
    f->used = 1;

    return fd;
}

int vfs_read(int fd, void* buf, uint32_t size) {
    task_t* curr = proc_current();
    file_t* f = proc_fd_get(curr, fd);
    if (!f || !f->used) return -1;
    if (!f->node->ops->read) return -1;

    int res = f->node->ops->read(f->node, f->offset, size, buf);
    if (res > 0) f->offset += res;
    return res;
}

int vfs_write(int fd, const void* buf, uint32_t size) {
    task_t* curr = proc_current();
    file_t* f = proc_fd_get(curr, fd);
    if (!f || !f->used) return -1;
    if (!f->node->ops->write) return -1;

    int res = f->node->ops->write(f->node, f->offset, size, buf);
    if (res > 0) f->offset += res;
    return res;
}

int vfs_ioctl(int fd, uint32_t req, void* arg) {
    task_t* curr = proc_current();
    file_t* f = proc_fd_get(curr, fd);
    if (!f || !f->used || !f->node || !f->node->ops) return -1;
    if (!f->node->ops->ioctl) return -1;
    return f->node->ops->ioctl(f->node, req, arg);
}

int vfs_close(int fd) {
    task_t* curr = proc_current();

    file_t f;
    memset(&f, 0, sizeof(f));
    if (proc_fd_remove(curr, fd, &f) < 0) return -1;

    vfs_node_t* node = f.node;

    if (node) {
        if (__sync_sub_and_fetch(&node->refs, 1) == 0) {
            if (node->ops && node->ops->close) {
                node->ops->close(node);
            } 
            else {
                kfree(node);
            }
        }
    }
    return 0;
}

vfs_node_t* vfs_create_node_from_path(const char* path) {
    int inode = yulafs_lookup(path);
    if (inode == -1) return 0;

    vfs_node_t* node = (vfs_node_t*)kmalloc(sizeof(vfs_node_t));
    if (!node) return 0;

    memset(node, 0, sizeof(vfs_node_t));
    node->inode_idx = inode;
    node->ops = &yfs_vfs_ops;
    node->flags = VFS_FLAG_EXEC_NODE | VFS_FLAG_YULAFS;
    
    yfs_inode_t info;
    if (yulafs_stat(inode, &info) == 0) {
        node->size = info.size;
    }
    strlcpy(node->name, path, sizeof(node->name));
    
    node->refs = 0;
    return node;
}