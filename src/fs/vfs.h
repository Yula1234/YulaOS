// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#ifndef FS_VFS_H
#define FS_VFS_H

#include <stdint.h>

#define VFS_FLAG_PIPE_READ   1u
#define VFS_FLAG_PIPE_WRITE  2u
#define VFS_FLAG_YULAFS      4u
#define VFS_FLAG_SHM         8u
#define VFS_FLAG_IPC_LISTEN  16u
#define VFS_FLAG_EXEC_NODE   0x80000000u

struct vfs_node;

typedef struct vfs_ops {
    int (*read)(struct vfs_node* node, uint32_t offset, uint32_t size, void* buffer);
    int (*write)(struct vfs_node* node, uint32_t offset, uint32_t size, const void* buffer);
    int (*open)(struct vfs_node* node);
    int (*close)(struct vfs_node* node);
    int (*ioctl)(struct vfs_node* node, uint32_t req, void* arg);
} vfs_ops_t;

typedef struct vfs_node {
    char name[32];
    uint32_t flags;
    uint32_t size;
    uint32_t inode_idx; 
    uint32_t refs;      
    vfs_ops_t* ops;     
    void* private_data; 
} vfs_node_t;

typedef struct {
    vfs_node_t* node;
    uint32_t offset;
    uint8_t  used;
} file_t;

int vfs_open(const char* path, int flags);
int vfs_read(int fd, void* buf, uint32_t size);
int vfs_write(int fd, const void* buf, uint32_t size);
int vfs_close(int fd);
int vfs_ioctl(int fd, uint32_t req, void* arg);
int vfs_getdents(int fd, void* buf, uint32_t size);
int vfs_fstatat(int dirfd, const char* name, void* stat_buf);

void devfs_register(vfs_node_t* node);
int devfs_unregister(const char* name);
vfs_node_t* devfs_fetch(const char* name);
vfs_node_t* vfs_create_node_from_path(const char* path);

#endif