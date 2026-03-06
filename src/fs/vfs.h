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
#define VFS_FLAG_DEVFS_ALLOC 32u
#define VFS_FLAG_PTY_MASTER  64u
#define VFS_FLAG_PTY_SLAVE   128u
#define VFS_FLAG_DEVFS_NODE  256u
#define VFS_FLAG_DEVFS_ROOT  512u
#define VFS_FLAG_INSTANCE_REF 0x40000000u
#define VFS_FLAG_EXEC_NODE   0x80000000u

struct vfs_node;

#ifdef __cplusplus
extern "C" {
#endif

typedef struct vfs_ops {
    int (*read)(struct vfs_node* node, uint32_t offset, uint32_t size, void* buffer);
    int (*write)(struct vfs_node* node, uint32_t offset, uint32_t size, const void* buffer);
    int (*open)(struct vfs_node* node);
    int (*close)(struct vfs_node* node);
    int (*ioctl)(struct vfs_node* node, uint32_t req, void* arg);
} vfs_ops_t;

#define VFS_OPEN_WRITE  1u
#define VFS_OPEN_APPEND 2u
#define VFS_OPEN_CREATE 4u
#define VFS_OPEN_TRUNC  8u

typedef struct {
    uint32_t type;
    uint32_t size;
    uint32_t inode;
    uint32_t flags;
    uint32_t created_at;
    uint32_t modified_at;
} __attribute__((packed)) vfs_stat_t;

typedef struct vfs_node {
    char name[32];
    uint32_t flags;
    uint32_t size;
    uint32_t inode_idx; 
    uint32_t refs;      
    const void* fs_driver;
    vfs_ops_t* ops;     
    void* private_data; 
    void (*private_retain)(void* private_data);
    void (*private_release)(void* private_data);
} vfs_node_t;

typedef struct {
    vfs_node_t* node;
    uint32_t offset;
    uint32_t flags;
    uint8_t  used;
} file_t;

#define FILE_FLAG_APPEND 1u

void vfs_init(void);

int vfs_mount(const char* mountpoint, const char* fs_name);
int vfs_umount(const char* mountpoint);

int vfs_open(const char* path, int flags);
int vfs_read(int fd, void* buf, uint32_t size);
int vfs_write(int fd, const void* buf, uint32_t size);
int vfs_close(int fd);
int vfs_ioctl(int fd, uint32_t req, void* arg);
int vfs_getdents(int fd, void* buf, uint32_t size);
int vfs_fstatat(int dirfd, const char* name, void* stat_buf);
int vfs_mkdir(const char* path);
int vfs_unlink(const char* path);
int vfs_stat_path(const char* path, vfs_stat_t* out);
int vfs_rename(const char* old_path, const char* new_path);
int vfs_get_fs_info(uint32_t* total_blocks, uint32_t* free_blocks, uint32_t* block_size);

int vfs_openat(int dirfd, const char* path, int flags);
int vfs_mkdirat(int dirfd, const char* path);
int vfs_unlinkat(int dirfd, const char* path);
int vfs_statat_path(int dirfd, const char* path, vfs_stat_t* out);
int vfs_renameat(int old_dirfd, const char* old_path, int new_dirfd, const char* new_path);

void vfs_node_retain(vfs_node_t* node);
void vfs_node_release(vfs_node_t* node);

void devfs_register(vfs_node_t* node);
int devfs_unregister(const char* name);
vfs_node_t* devfs_fetch(const char* name);
vfs_node_t* devfs_clone(const char* name);
vfs_node_t* devfs_take(const char* name);
vfs_node_t* vfs_create_node_from_path(const char* path);

#ifdef __cplusplus
}
#endif

#endif
