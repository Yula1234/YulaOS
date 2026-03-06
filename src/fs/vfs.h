/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2025 Yula1234 */

#ifndef FS_VFS_H
#define FS_VFS_H

#include <stdint.h>

/*
 * Virtual File System.
 *
 * This header defines the common node and operation ABI used by all file
 * system backends and pseudo filesystems.
 *
 * Core model:
 *  - `vfs_node_t` is a lightweight handle describing an openable object.
 *  - `vfs_ops_t` is the operation table. A backend can implement any subset.
 *  - nodes are reference counted via vfs_node_retain()/vfs_node_release().
 *
 * Ownership:
 *  - `private_data` is owned by the backend and is retained/released through
 *    `private_retain` / `private_release` when nodes are cloned or dropped.
 */

/*
 * Node flags.
 *
 * Flags are backend-defined hints consumed by VFS and by higher-level
 * subsystems (poll, pipe, IPC, devfs).
 */
/* Read endpoint of a pipe node. */
#define VFS_FLAG_PIPE_READ    1u
/* Write endpoint of a pipe node. */
#define VFS_FLAG_PIPE_WRITE   2u
/* Node belongs to the yulafs backend. */
#define VFS_FLAG_YULAFS       4u
/* Shared memory node. */
#define VFS_FLAG_SHM          8u
/* IPC listen endpoint. */
#define VFS_FLAG_IPC_LISTEN   16u
/* Node structure was allocated by devfs/VFS and must be freed on release. */
#define VFS_FLAG_DEVFS_ALLOC  32u
/* PTY master endpoint. */
#define VFS_FLAG_PTY_MASTER   64u
/* PTY slave endpoint. */
#define VFS_FLAG_PTY_SLAVE    128u
/* Node is registered in devfs. */
#define VFS_FLAG_DEVFS_NODE   256u
/* Node is the devfs root directory. */
#define VFS_FLAG_DEVFS_ROOT   512u

/*
 * When set, `fs_driver` is a retained reference to an internal filesystem
 * instance object.
 */
#define VFS_FLAG_INSTANCE_REF 0x40000000u

/*
 * Marks a node that can be executed.
 * The exact interpretation is filesystem-specific.
 */
#define VFS_FLAG_EXEC_NODE    0x80000000u

struct vfs_node;

#ifdef __cplusplus
extern "C" {
#endif

typedef struct vfs_ops {
    /*
     * Ops return a non-negative result on success (typically bytes transferred)
     * and a negative value on failure.
     *
     * For stream-like nodes, `offset` is ignored by the backend.
     * For file-like nodes, `offset` is the byte offset.
     */
    int (*read)(
        struct vfs_node* node,
        uint32_t offset,
        uint32_t size,
        void* buffer
    );
    int (*write)(
        struct vfs_node* node,
        uint32_t offset,
        uint32_t size,
        const void* buffer
    );
    int (*open)(struct vfs_node* node);
    int (*close)(struct vfs_node* node);
    int (*ioctl)(struct vfs_node* node, uint32_t req, void* arg);
} vfs_ops_t;

/* Flags passed to vfs_open/vfs_openat. */
#define VFS_OPEN_WRITE  1u
#define VFS_OPEN_APPEND 2u
#define VFS_OPEN_CREATE 4u
#define VFS_OPEN_TRUNC  8u

typedef struct {
    /*
     * `type` and `inode` are filesystem-defined.
     * `size` is in bytes.
     * `flags` are filesystem-defined.
     * `created_at` / `modified_at` are filesystem-defined timestamps.
     */
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

    /*
     * File size in bytes.
     * For stream-like nodes this can be 0 or backend-defined.
     */
    uint32_t size;

    /* Filesystem-defined inode or object identifier. */
    uint32_t inode_idx;

    /* Reference count for this node object. Managed by vfs_node_retain/release. */
    uint32_t refs;

    /*
     * Filesystem driver identity.
     *
     * Typically points to an internal filesystem instance. If
     * VFS_FLAG_INSTANCE_REF is set, VFS owns a retained reference.
     */
    const void* fs_driver;

    /* Operation table for this node. May be null. */
    vfs_ops_t* ops;

    /* Backend-private payload for ops. */
    void* private_data;

    /*
     * Backend-private ownership hooks.
     *
     * If provided:
     *  - retain is called before a node clone starts sharing `private_data`
     *  - release is called when the last node reference is dropped
     */
    void (*private_retain)(void* private_data);
    void (*private_release)(void* private_data);
} vfs_node_t;

typedef struct {
    /*
     * Per-process open file state.
     *
     * `node` is a retained reference while `used` is set.
     */
    vfs_node_t* node;

    /* Byte offset for sequential reads/writes. */
    uint32_t offset;

    /* Per-descriptor flags (FILE_FLAG_*). */
    uint32_t flags;
    uint8_t  used;
} file_t;

/*
 * When set, writes are performed at the end of file and advance the offset.
 * This is derived from VFS_OPEN_APPEND.
 */
#define FILE_FLAG_APPEND 1u

void vfs_init(void);

/* Mount management. */
int vfs_mount(const char* mountpoint, const char* fs_name);
int vfs_umount(const char* mountpoint);

/* Process file descriptor API. */
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
int vfs_get_fs_info(
    uint32_t* total_blocks,
    uint32_t* free_blocks,
    uint32_t* block_size
);

int vfs_openat(int dirfd, const char* path, int flags);
int vfs_mkdirat(int dirfd, const char* path);
int vfs_unlinkat(int dirfd, const char* path);
int vfs_statat_path(int dirfd, const char* path, vfs_stat_t* out);
int vfs_renameat(int old_dirfd, const char* old_path, int new_dirfd, const char* new_path);

/* Node reference management. */
void vfs_node_retain(vfs_node_t* node);
void vfs_node_release(vfs_node_t* node);

/* DevFS registry helpers. */
void devfs_register(vfs_node_t* node);
int devfs_unregister(const char* name);

/*
 * devfs_fetch returns a borrowed pointer.
 * devfs_clone returns a new node with refs == 1.
 * devfs_take removes the node from the registry and returns it to the caller.
 */
vfs_node_t* devfs_fetch(const char* name);
vfs_node_t* devfs_clone(const char* name);
vfs_node_t* devfs_take(const char* name);
vfs_node_t* vfs_create_node_from_path(const char* path);

#ifdef __cplusplus
}
#endif

#endif
