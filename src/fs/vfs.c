// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#include "vfs.h"
#include "yulafs.h"
#include "../kernel/proc.h"
#include "../lib/string.h"
#include "../mm/heap.h"

static vfs_node_t* dev_nodes[16];
static int dev_count = 0;

void devfs_register(vfs_node_t* node) {
    if (dev_count < 16) dev_nodes[dev_count++] = node;
}

vfs_node_t* devfs_fetch(const char* name) {
    for (int i = 0; i < dev_count; i++) {
        if (name[0] == dev_nodes[i]->name[0] && strcmp(dev_nodes[i]->name, name) == 0) return dev_nodes[i];
    }
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
    node->flags = VFS_FLAG_EXEC_NODE;
    
    yfs_inode_t info;
    if (yulafs_stat(inode, &info) == 0) {
        node->size = info.size;
    }
    strlcpy(node->name, path, sizeof(node->name));
    
    node->refs = 0;
    return node;
}