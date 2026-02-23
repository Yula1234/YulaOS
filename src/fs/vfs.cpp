// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#include "vfs.h"

extern "C" {

#include "yulafs.h"

}
#include "../kernel/proc.h"
#include "../lib/string.h"
#include "../mm/heap.h"
#include <hal/lock.h>

#include <lib/hash_map.h>
#include <lib/cpp/lock_guard.h>
#include <lib/cpp/string.h>

namespace {

class FileDescHandle {
public:
    FileDescHandle(task_t* task, int fd) noexcept
        : desc_(proc_fd_get(task, fd)) {
    }

    FileDescHandle(const FileDescHandle&) = delete;
    FileDescHandle& operator=(const FileDescHandle&) = delete;

    ~FileDescHandle() {
        if (desc_) {
            file_desc_release(desc_);
        }
    }

    file_desc_t* get() const noexcept {
        return desc_;
    }

    explicit operator bool() const noexcept {
        return desc_ != nullptr;
    }

private:
    file_desc_t* desc_;
};

static vfs_node_t* vfs_node_clone_existing(const vfs_node_t* src) noexcept {
    if (!src) {
        return nullptr;
    }

    vfs_node_t snapshot;
    memset(&snapshot, 0, sizeof(snapshot));

    memcpy(&snapshot, src, sizeof(snapshot));

    if (snapshot.private_retain && snapshot.private_data) {
        snapshot.private_retain(snapshot.private_data);
    }

    auto* node = static_cast<vfs_node_t*>(kmalloc(sizeof(vfs_node_t)));
    if (!node) {
        if (snapshot.private_release && snapshot.private_data) {
            snapshot.private_release(snapshot.private_data);
        }

        return nullptr;
    }

    memcpy(node, &snapshot, sizeof(*node));
    node->refs = 1;
    node->flags |= VFS_FLAG_DEVFS_ALLOC;
    return node;
}

class DevFSRegistry {
public:
    void register_node(vfs_node_t* node) noexcept;
    int unregister_node(const char* name) noexcept;
    vfs_node_t* fetch_borrowed(const char* name) noexcept;
    vfs_node_t* clone(const char* name) noexcept;
    vfs_node_t* take(const char* name) noexcept;

private:
    kernel::SpinLock lock_;
    HashMap<kernel::string, vfs_node_t*, 256> nodes_;
};

static DevFSRegistry g_devfs;

static DevFSRegistry& devfs_registry() noexcept {
    return g_devfs;
}

} // namespace

namespace {

void DevFSRegistry::register_node(vfs_node_t* node) noexcept {
    if (!node || node->name[0] == '\0') {
        return;
    }

    const kernel::string key(node->name);

    kernel::SpinLockSafeGuard guard(lock_);
    nodes_.insert_or_assign(key, node);
}

int DevFSRegistry::unregister_node(const char* name) noexcept {
    if (!name || name[0] == '\0') {
        return -1;
    }

    const kernel::string key(name);

    kernel::SpinLockSafeGuard guard(lock_);
    return nodes_.remove(key) ? 0 : -1;
}

vfs_node_t* DevFSRegistry::fetch_borrowed(const char* name) noexcept {
    if (!name || name[0] == '\0') {
        return nullptr;
    }

    const kernel::string key(name);

    kernel::SpinLockSafeGuard guard(lock_);
    auto locked = nodes_.find_ptr(key);
    if (!locked) {
        return nullptr;
    }

    auto* value = locked.value_ptr();
    return value ? *value : nullptr;
}

vfs_node_t* DevFSRegistry::take(const char* name) noexcept {
    if (!name || name[0] == '\0') {
        return nullptr;
    }

    const kernel::string key(name);
    vfs_node_t* out = nullptr;

    kernel::SpinLockSafeGuard guard(lock_);
    if (!nodes_.remove_and_get(key, out)) {
        return nullptr;
    }

    return out;
}

vfs_node_t* DevFSRegistry::clone(const char* name) noexcept {
    if (!name || name[0] == '\0') {
        return nullptr;
    }

    const kernel::string key(name);

    vfs_node_t snapshot;
    memset(&snapshot, 0, sizeof(snapshot));

    {
        kernel::SpinLockSafeGuard guard(lock_);

        auto locked = nodes_.find_ptr(key);
        if (!locked) {
            return nullptr;
        }

        vfs_node_t** src_ptr = locked.value_ptr();
        if (!src_ptr || !*src_ptr) {
            return nullptr;
        }

        vfs_node_t* src = *src_ptr;

        memcpy(&snapshot, src, sizeof(snapshot));

        if (snapshot.private_retain && snapshot.private_data) {
            snapshot.private_retain(snapshot.private_data);
        }
    }

    auto* node = static_cast<vfs_node_t*>(kmalloc(sizeof(vfs_node_t)));
    if (!node) {

        if (snapshot.private_release && snapshot.private_data) {
            snapshot.private_release(snapshot.private_data);
        }

        return nullptr;
    }

    memcpy(node, &snapshot, sizeof(*node));
    node->refs = 1;
    node->flags |= VFS_FLAG_DEVFS_ALLOC;

    return node;
}

} // namespace

void devfs_register(vfs_node_t* node) {
    devfs_registry().register_node(node);
}

int devfs_unregister(const char* name) {
    return devfs_registry().unregister_node(name);
}

vfs_node_t* devfs_fetch(const char* name) {
    return devfs_registry().fetch_borrowed(name);
}

vfs_node_t* devfs_clone(const char* name) {
    return devfs_registry().clone(name);
}

vfs_node_t* devfs_take(const char* name) {
    return devfs_registry().take(name);
}

void vfs_node_retain(vfs_node_t* node) {
    if (!node) {
        return;
    }

    __sync_fetch_and_add(&node->refs, 1);
}

void vfs_node_release(vfs_node_t* node) {
    if (!node) {
        return;
    }

    if (__sync_sub_and_fetch(&node->refs, 1) != 0) {
        return;
    }

    if (node->ops && node->ops->close) {
        (void)node->ops->close(node);
    }

    if (node->private_release && node->private_data) {
        node->private_release(node->private_data);
        node->private_data = 0;
    }

    kfree(node);
}

int vfs_getdents(int fd, void* buf, uint32_t size) {
    task_t* curr = proc_current();
    FileDescHandle d(curr, fd);

    if (!d || !d.get()->node) {
        return -1;
    }

    if (!buf || size == 0) {
        return -1;
    }

    vfs_node_t* node = d.get()->node;

    if (!node) {
        return -1;
    }

    if ((node->flags & VFS_FLAG_YULAFS) == 0) {
        return -1;
    }

    kernel::SpinLockNativeSafeGuard guard(d.get()->lock);

    return yulafs_getdents(
        node->inode_idx,
        &d.get()->offset,
        (yfs_dirent_info_t*)buf,
        size
    );
}

typedef struct {
    uint32_t type;
    uint32_t size;
} __attribute__((packed)) vfs_stat_t;

int vfs_fstatat(int dirfd, const char* name, void* stat_buf) {
    task_t* curr = proc_current();
    FileDescHandle d(curr, dirfd);

    if (!d || !d.get()->node) {
        return -1;
    }

    if (!name || !*name || !stat_buf) {
        return -1;
    }

    vfs_node_t* node = d.get()->node;

    if (!node) {
        return -1;
    }

    if ((node->flags & VFS_FLAG_YULAFS) == 0) {
        return -1;
    }

    int ino = yulafs_lookup_in_dir(node->inode_idx, name);
    if (ino < 0) {
        return -1;
    }

    yfs_inode_t info;
    if (yulafs_stat((yfs_ino_t)ino, &info) != 0) {
        return -1;
    }

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
    yfs_read_wrapper,
    yfs_write_wrapper,
    0,
    0,
    0,
};

int vfs_open(const char* path, int flags) {
    task_t* curr = proc_current();
    if (!curr) {
        return -1;
    }

    if ((flags & ~3) != 0) {
        return -1;
    }

    const int open_append = (flags & 2) != 0;
    const int open_write = ((flags & 1) != 0) || open_append;

    vfs_node_t* node = 0;

    const char* target_path = path;

    if (path[0] == '/' && path[1] != '\0') {
        target_path++;
    }

    if (strncmp(target_path, "dev/", 4) == 0) {
        const char* dev_name = target_path + 4;
        if (strcmp(dev_name, "tty") == 0) {
            node = vfs_node_clone_existing(curr->controlling_tty);
        } else {
            node = devfs_clone(dev_name);
        }
    } else {
        int inode = yulafs_lookup(path);

        if (inode == -1 && open_write) {
            inode = yulafs_create(path);
        }

        if (inode != -1) {
            if (open_write && !open_append) {
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

    if (!node) {
        return -1;
    }

    node->refs = 1;

    if (node->ops && node->ops->open) {
        int rc = node->ops->open(node);
        if (rc != 0) {
            vfs_node_release(node);
            return -1;
        }
    }

    file_desc_t* d = 0;
    int fd = proc_fd_alloc(curr, &d);
    if (fd < 0 || !d) {
        vfs_node_release(node);
        return -1;
    }

    d->node = node;
    d->offset = 0;
    d->flags = open_append ? FILE_FLAG_APPEND : 0u;

    return fd;
}

int vfs_read(int fd, void* buf, uint32_t size) {
    task_t* curr = proc_current();
    FileDescHandle d(curr, fd);

    if (!d || !d.get()->node) {
        return -1;
    }

    if (!d.get()->node->ops || !d.get()->node->ops->read) {
        return -1;
    }

    uint32_t off;
    {
        kernel::SpinLockNativeSafeGuard guard(d.get()->lock);
        off = d.get()->offset;
    }

    const int res = d.get()->node->ops->read(d.get()->node, off, size, buf);
    if (res > 0) {
        kernel::SpinLockNativeSafeGuard guard(d.get()->lock);
        d.get()->offset = off + (uint32_t)res;
    }

    return res;
}

int vfs_write(int fd, const void* buf, uint32_t size) {
    task_t* curr = proc_current();
    FileDescHandle d(curr, fd);

    if (!d || !d.get()->node) {
        return -1;
    }

    if (!d.get()->node->ops || !d.get()->node->ops->write) {
        return -1;
    }

    uint32_t fflags;
    uint32_t off;
    {
        kernel::SpinLockNativeSafeGuard guard(d.get()->lock);
        fflags = d.get()->flags;
        off = d.get()->offset;
    }

    if ((fflags & FILE_FLAG_APPEND) != 0 && (d.get()->node->flags & VFS_FLAG_YULAFS) != 0) {
        yfs_off_t start = 0;

        const int res = yulafs_append(d.get()->node->inode_idx, buf, size, &start);
        if (res > 0) {
            kernel::SpinLockNativeSafeGuard guard(d.get()->lock);
            d.get()->offset = (uint32_t)start + (uint32_t)res;
        }

        return res;
    }

    const int res = d.get()->node->ops->write(d.get()->node, off, size, buf);
    if (res > 0) {
        kernel::SpinLockNativeSafeGuard guard(d.get()->lock);
        d.get()->offset = off + (uint32_t)res;
    }

    return res;
}

int vfs_ioctl(int fd, uint32_t req, void* arg) {
    task_t* curr = proc_current();
    FileDescHandle d(curr, fd);

    if (!d || !d.get()->node || !d.get()->node->ops) {
        return -1;
    }

    if (!d.get()->node->ops->ioctl) {
        return -1;
    }

    return d.get()->node->ops->ioctl(d.get()->node, req, arg);
}

int vfs_close(int fd) {
    task_t* curr = proc_current();

    file_desc_t* d = 0;

    if (proc_fd_remove(curr, fd, &d) < 0 || !d) {
        return -1;
    }

    file_desc_release(d);
    return 0;
}

vfs_node_t* vfs_create_node_from_path(const char* path) {
    int inode = yulafs_lookup(path);

    if (inode == -1) {
        return 0;
    }

    vfs_node_t* node = (vfs_node_t*)kmalloc(sizeof(vfs_node_t));

    if (!node) {
        return 0;
    }

    memset(node, 0, sizeof(vfs_node_t));
    node->inode_idx = inode;
    node->ops = &yfs_vfs_ops;
    node->flags = VFS_FLAG_EXEC_NODE | VFS_FLAG_YULAFS;

    yfs_inode_t info;
    if (yulafs_stat(inode, &info) == 0) {
        node->size = info.size;
    }
    strlcpy(node->name, path, sizeof(node->name));

    node->refs = 1;
    return node;
}
