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
    node->flags |= VFS_FLAG_DEVFS_NODE;
    return node;
}

class DevFSRegistry {
public:
    void register_node(vfs_node_t* node) noexcept;
    int unregister_node(const char* name) noexcept;
    vfs_node_t* fetch_borrowed(const char* name) noexcept;
    vfs_node_t* clone(const char* name) noexcept;
    vfs_node_t* take(const char* name) noexcept;
    int getdents(uint32_t* inout_offset, yfs_dirent_info_t* out, uint32_t out_size) noexcept;

private:
    kernel::SpinLock lock_;
    HashMap<kernel::string, vfs_node_t*, 256> nodes_;
};

static DevFSRegistry g_devfs;

static DevFSRegistry& devfs_registry() noexcept {
    return g_devfs;
}

} // namespace

struct vfs_fs_driver;

struct vfs_fs_driver_ops {
    vfs_node_t* (*open)(task_t* curr, const char* mountpoint, const char* rel, int is_abs, int flags);
    int (*mkdir)(const char* mountpoint, const char* rel, int is_abs);
    int (*unlink)(const char* mountpoint, const char* rel, int is_abs);
    int (*rename)(
        const char* old_mountpoint,
        const char* old_rel,
        const char* new_mountpoint,
        const char* new_rel,
        int old_is_abs,
        int new_is_abs
    );
    int (*stat)(const char* mountpoint, const char* rel, int is_abs, vfs_stat_t* out);
    int (*getdents)(task_t* curr, vfs_node_t* dir_node, uint32_t* inout_offset, yfs_dirent_info_t* out, uint32_t out_size);
    int (*fstatat)(task_t* curr, vfs_node_t* dir_node, const char* name, vfs_stat_t* out);
    int (*get_fs_info)(uint32_t* total_blocks, uint32_t* free_blocks, uint32_t* block_size);
    vfs_node_t* (*create_node_from_path)(const char* mountpoint, const char* rel, int is_abs);
};

struct vfs_fs_driver {
    const char* name;
    const vfs_fs_driver_ops* ops;
};

struct vfs_mount_entry {
    const vfs_fs_driver* driver;
    char mountpoint[32];
    uint8_t used;
};

extern "C" {
extern const vfs_fs_driver g_yulafs_driver;
extern const vfs_fs_driver g_devfs_driver;
}

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
    node->flags |= VFS_FLAG_DEVFS_NODE;

    return node;
}

int DevFSRegistry::getdents(uint32_t* inout_offset, yfs_dirent_info_t* out, uint32_t out_size) noexcept {
    if (!inout_offset || !out || out_size < sizeof(yfs_dirent_info_t)) {
        return -1;
    }

    const uint32_t max_entries = out_size / (uint32_t)sizeof(yfs_dirent_info_t);
    uint32_t written = 0;
    uint32_t idx = *inout_offset;

    auto view = nodes_.locked_view();
    for (auto it = view.begin(); it != view.end() && written < max_entries; ++it) {
        if (idx > 0) {
            idx--;
            continue;
        }

        auto kv = *it;
        vfs_node_t* tmpl = kv.second;
        if (!tmpl) {
            continue;
        }

        yfs_dirent_info_t* dst = &out[written];
        memset(dst, 0, sizeof(*dst));

        dst->inode = 0;
        dst->type = YFS_TYPE_FILE;
        dst->size = tmpl->size;
        strlcpy(dst->name, tmpl->name, sizeof(dst->name));

        written++;
        (*inout_offset)++;
    }

    return (int)(written * (uint32_t)sizeof(yfs_dirent_info_t));
}

} // namespace

extern "C" {

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

static void vfs_init_impl(void);
static int vfs_mount_impl(const char* mountpoint, const char* fs_name);
static int vfs_umount_impl(const char* mountpoint);

void vfs_init(void) {
    vfs_init_impl();
}

int vfs_mount(const char* mountpoint, const char* fs_name) {
    return vfs_mount_impl(mountpoint, fs_name);
}

int vfs_umount(const char* mountpoint) {
    return vfs_umount_impl(mountpoint);
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

    const vfs_fs_driver* driver = nullptr;
    if ((node->flags & VFS_FLAG_DEVFS_ROOT) != 0 || (node->flags & VFS_FLAG_DEVFS_NODE) != 0) {
        driver = &g_devfs_driver;
    } else if ((node->flags & VFS_FLAG_YULAFS) != 0) {
        driver = &g_yulafs_driver;
    }

    if (!driver || !driver->ops || !driver->ops->getdents) {
        return -1;
    }

    kernel::SpinLockNativeSafeGuard guard(d.get()->lock);
    return driver->ops->getdents(curr, node, &d.get()->offset, (yfs_dirent_info_t*)buf, size);
}

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

    const vfs_fs_driver* driver = nullptr;
    if ((node->flags & VFS_FLAG_DEVFS_ROOT) != 0 || (node->flags & VFS_FLAG_DEVFS_NODE) != 0) {
        driver = &g_devfs_driver;
    } else if ((node->flags & VFS_FLAG_YULAFS) != 0) {
        driver = &g_yulafs_driver;
    }

    if (!driver || !driver->ops || !driver->ops->fstatat) {
        return -1;
    }

    return driver->ops->fstatat(curr, node, name, (vfs_stat_t*)stat_buf);
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

static vfs_mount_entry g_mounts[8];

static const vfs_fs_driver* vfs_driver_from_name(const char* fs_name);

static int vfs_mountpoint_is_valid(const char* mountpoint) {
    if (!mountpoint || mountpoint[0] != '/') {
        return 0;
    }

    if (mountpoint[1] == '\0') {
        return 1;
    }

    const size_t n = strlen(mountpoint);
    if (n == 0 || n >= sizeof(g_mounts[0].mountpoint)) {
        return 0;
    }

    if (mountpoint[n - 1] == '/') {
        return 0;
    }

    return 1;
}

static int vfs_path_is_abs(const char* path) {
    return path && path[0] == '/';
}

static int vfs_mount_match_len(const char* path, const char* mountpoint) {
    if (!path || !mountpoint) {
        return -1;
    }

    if (mountpoint[0] != '/') {
        return -1;
    }

    if (mountpoint[1] == '\0') {
        return 1;
    }

    const size_t mlen = strlen(mountpoint);
    if (strncmp(path, mountpoint, mlen) != 0) {
        return -1;
    }

    const char next = path[mlen];
    if (next == '\0' || next == '/') {
        return (int)mlen;
    }

    return -1;
}

static int vfs_build_abs_path(
    const char* mountpoint,
    const char* rel,
    char* out,
    size_t out_size
) {
    if (!mountpoint || mountpoint[0] != '/' || !out || out_size == 0) {
        return -1;
    }

    if (!rel || rel[0] == '\0') {
        return strlcpy(out, mountpoint, out_size) >= out_size ? -1 : 0;
    }

    if (strcmp(mountpoint, "/") == 0) {
        if (out_size < 2) {
            return -1;
        }

        out[0] = '/';
        out[1] = '\0';

        const size_t n = strlcat(out, rel, out_size);
        return n >= out_size ? -1 : 0;
    }

    const size_t n0 = strlcpy(out, mountpoint, out_size);
    if (n0 >= out_size) {
        return -1;
    }

    if (out[n0 - 1] != '/') {
        const size_t n1 = strlcat(out, "/", out_size);
        if (n1 >= out_size) {
            return -1;
        }
    }

    const size_t n2 = strlcat(out, rel, out_size);
    return n2 >= out_size ? -1 : 0;
}

static int vfs_mount_table_insert(const char* mountpoint, const vfs_fs_driver* driver) {
    for (size_t i = 0; i < sizeof(g_mounts) / sizeof(g_mounts[0]); i++) {
        if (!g_mounts[i].used) {
            memset(&g_mounts[i], 0, sizeof(g_mounts[i]));
            g_mounts[i].used = 1u;
            g_mounts[i].driver = driver;
            strlcpy(g_mounts[i].mountpoint, mountpoint, sizeof(g_mounts[i].mountpoint));
            return 0;
        }
    }

    return -1;
}

static int vfs_mount_table_remove(const char* mountpoint) {
    for (size_t i = 0; i < sizeof(g_mounts) / sizeof(g_mounts[0]); i++) {
        if (!g_mounts[i].used) {
            continue;
        }

        if (strcmp(g_mounts[i].mountpoint, mountpoint) == 0) {
            memset(&g_mounts[i], 0, sizeof(g_mounts[i]));
            return 0;
        }
    }

    return -1;
}

static int vfs_mount_table_has(const char* mountpoint) {
    if (!mountpoint) {
        return -1;
    }

    for (size_t i = 0; i < sizeof(g_mounts) / sizeof(g_mounts[0]); i++) {
        if (!g_mounts[i].used) {
            continue;
        }

        if (strcmp(g_mounts[i].mountpoint, mountpoint) == 0) {
            return 0;
        }
    }

    return -1;
}

static int vfs_resolve_mount(
    const char* path,
    const vfs_mount_entry** out_mount,
    const char** out_rel,
    int* out_is_abs
) {
    if (!path || path[0] == '\0' || !out_mount || !out_rel || !out_is_abs) {
        return -1;
    }

    const int is_abs = vfs_path_is_abs(path);
    *out_is_abs = is_abs;

    if (!is_abs) {
        for (size_t i = 0; i < sizeof(g_mounts) / sizeof(g_mounts[0]); i++) {
            if (!g_mounts[i].used) {
                continue;
            }

            if (strcmp(g_mounts[i].mountpoint, "/") == 0) {
                if (!g_mounts[i].driver || !g_mounts[i].driver->ops) {
                    return -1;
                }

                *out_mount = &g_mounts[i];
                *out_rel = path;
                return 0;
            }
        }

        return -1;
    }

    int best_len = -1;
    const vfs_mount_entry* best = nullptr;
    const char* best_mount = nullptr;

    for (size_t i = 0; i < sizeof(g_mounts) / sizeof(g_mounts[0]); i++) {
        if (!g_mounts[i].used) {
            continue;
        }

        const int mlen = vfs_mount_match_len(path, g_mounts[i].mountpoint);
        if (mlen > best_len) {
            best_len = mlen;
            best = &g_mounts[i];
            best_mount = g_mounts[i].mountpoint;
        }
    }

    if (best_len < 0 || !best_mount || !best || !best->driver || !best->driver->ops) {
        return -1;
    }

    const size_t mlen = strlen(best_mount);
    const char* rel = path + mlen;
    if (rel[0] == '/') {
        rel++;
    }

    *out_mount = best;
    *out_rel = rel;
    return 0;
}

static int vfs_yulafs_getdents(
    task_t* curr,
    vfs_node_t* dir_node,
    uint32_t* inout_offset,
    yfs_dirent_info_t* out,
    uint32_t out_size
) {
    (void)curr;

    if (!dir_node || (dir_node->flags & VFS_FLAG_YULAFS) == 0) {
        return -1;
    }

    return yulafs_getdents(dir_node->inode_idx, inout_offset, out, out_size);
}

static int vfs_yulafs_fstatat(task_t* curr, vfs_node_t* dir_node, const char* name, vfs_stat_t* out) {
    (void)curr;

    if (!dir_node || (dir_node->flags & VFS_FLAG_YULAFS) == 0 || !name || !*name || !out) {
        return -1;
    }

    const int ino = yulafs_lookup_in_dir(dir_node->inode_idx, name);
    if (ino < 0) {
        return -1;
    }

    yfs_inode_t info;
    if (yulafs_stat((yfs_ino_t)ino, &info) != 0) {
        return -1;
    }

    out->type = info.type;
    out->size = info.size;
    return 0;
}

static int vfs_mount_impl(const char* mountpoint, const char* fs_name) {
    if (!mountpoint || !fs_name) {
        return -1;
    }

    if (!vfs_mountpoint_is_valid(mountpoint)) {
        return -1;
    }

    if (vfs_mount_table_has(mountpoint) == 0) {
        return -1;
    }

    const vfs_fs_driver* driver = vfs_driver_from_name(fs_name);
    if (!driver || !driver->ops) {
        return -1;
    }

    if (vfs_mount_table_insert(mountpoint, driver) != 0) {
        return -1;
    }

    return 0;
}

static int vfs_umount_impl(const char* mountpoint) {
    if (!mountpoint) {
        return -1;
    }

    if (!vfs_mountpoint_is_valid(mountpoint)) {
        return -1;
    }

    if (vfs_mount_table_remove(mountpoint) != 0) {
        return -1;
    }

    return 0;
}

static void vfs_init_impl(void) {
    memset(g_mounts, 0, sizeof(g_mounts));

    (void)vfs_mount_impl("/", "yulafs");
    (void)vfs_mount_impl("/dev", "devfs");
}

static vfs_node_t* vfs_yulafs_open(task_t* curr, const char* mountpoint, const char* rel, int is_abs, int flags);
static int vfs_yulafs_mkdir(const char* mountpoint, const char* rel, int is_abs);
static int vfs_yulafs_unlink(const char* mountpoint, const char* rel, int is_abs);
static int vfs_yulafs_rename(
    const char* old_mountpoint,
    const char* old_rel,
    const char* new_mountpoint,
    const char* new_rel,
    int old_is_abs,
    int new_is_abs
);
static int vfs_yulafs_stat(const char* mountpoint, const char* rel, int is_abs, vfs_stat_t* out);
static int vfs_yulafs_getdents(task_t* curr, vfs_node_t* dir_node, uint32_t* inout_offset, yfs_dirent_info_t* out, uint32_t out_size);
static int vfs_yulafs_fstatat(task_t* curr, vfs_node_t* dir_node, const char* name, vfs_stat_t* out);
static int vfs_yulafs_get_fs_info(uint32_t* total_blocks, uint32_t* free_blocks, uint32_t* block_size);
static vfs_node_t* vfs_yulafs_create_node_from_path(const char* mountpoint, const char* rel, int is_abs);

static vfs_node_t* vfs_devfs_open(task_t* curr, const char* mountpoint, const char* rel, int is_abs, int flags);
static int vfs_devfs_stat(const char* mountpoint, const char* rel, int is_abs, vfs_stat_t* out);
static int vfs_devfs_getdents(task_t* curr, vfs_node_t* dir_node, uint32_t* inout_offset, yfs_dirent_info_t* out, uint32_t out_size);
static int vfs_devfs_fstatat(task_t* curr, vfs_node_t* dir_node, const char* name, vfs_stat_t* out);
static vfs_node_t* vfs_devfs_create_node_from_path(const char* mountpoint, const char* rel, int is_abs);

static const vfs_fs_driver_ops g_yulafs_ops = {
    vfs_yulafs_open,
    vfs_yulafs_mkdir,
    vfs_yulafs_unlink,
    vfs_yulafs_rename,
    vfs_yulafs_stat,
    vfs_yulafs_getdents,
    vfs_yulafs_fstatat,
    vfs_yulafs_get_fs_info,
    vfs_yulafs_create_node_from_path,
};

const vfs_fs_driver g_yulafs_driver = {
    "yulafs",
    &g_yulafs_ops,
};

static const vfs_fs_driver_ops g_devfs_ops = {
    vfs_devfs_open,
    0,
    0,
    0,
    vfs_devfs_stat,
    vfs_devfs_getdents,
    vfs_devfs_fstatat,
    0,
    vfs_devfs_create_node_from_path,
};

const vfs_fs_driver g_devfs_driver = {
    "devfs",
    &g_devfs_ops,
};

static const vfs_fs_driver* vfs_driver_from_name(const char* fs_name) {
    if (!fs_name) {
        return nullptr;
    }

    if (strcmp(fs_name, g_yulafs_driver.name) == 0) {
        return &g_yulafs_driver;
    }

    if (strcmp(fs_name, g_devfs_driver.name) == 0) {
        return &g_devfs_driver;
    }

    return nullptr;
}

static vfs_node_t* vfs_open_devfs_node(task_t* curr, const char* dev_name) {
    if (!curr || !dev_name || dev_name[0] == '\0') {
        return nullptr;
    }

    if (strcmp(dev_name, "tty") == 0) {
        return vfs_node_clone_existing(curr->controlling_tty);
    }

    return devfs_clone(dev_name);
}

static vfs_node_t* vfs_open_devfs_root_node(void) {
    auto* node = static_cast<vfs_node_t*>(kmalloc(sizeof(vfs_node_t)));
    if (!node) {
        return nullptr;
    }

    memset(node, 0, sizeof(vfs_node_t));
    strlcpy(node->name, "/dev", sizeof(node->name));
    node->flags = VFS_FLAG_DEVFS_ROOT;
    node->refs = 1;
    return node;
}

static vfs_node_t* vfs_open_yulafs_node(const char* path, int open_write, int open_append);

static vfs_node_t* vfs_yulafs_open(task_t* curr, const char* mountpoint, const char* rel, int is_abs, int flags) {
    (void)curr;

    const int open_append = (flags & 2) != 0;
    const int open_write = ((flags & 1) != 0) || open_append;

    if (!is_abs) {
        return vfs_open_yulafs_node(rel, open_write, open_append);
    }

    char abs_path[128];
    if (vfs_build_abs_path(mountpoint, rel, abs_path, sizeof(abs_path)) != 0) {
        return nullptr;
    }

    return vfs_open_yulafs_node(abs_path, open_write, open_append);
}

static int vfs_yulafs_mkdir(const char* mountpoint, const char* rel, int is_abs) {
    if (!rel) {
        return -1;
    }

    if (!is_abs) {
        return yulafs_mkdir(rel);
    }

    char abs_path[128];
    if (vfs_build_abs_path(mountpoint, rel, abs_path, sizeof(abs_path)) != 0) {
        return -1;
    }

    return yulafs_mkdir(abs_path);
}

static int vfs_yulafs_unlink(const char* mountpoint, const char* rel, int is_abs) {
    if (!rel) {
        return -1;
    }

    if (!is_abs) {
        return yulafs_unlink(rel);
    }

    char abs_path[128];
    if (vfs_build_abs_path(mountpoint, rel, abs_path, sizeof(abs_path)) != 0) {
        return -1;
    }

    return yulafs_unlink(abs_path);
}

static int vfs_yulafs_rename(
    const char* old_mountpoint,
    const char* old_rel,
    const char* new_mountpoint,
    const char* new_rel,
    int old_is_abs,
    int new_is_abs
) {
    if (!old_rel || !new_rel) {
        return -1;
    }

    if (!old_is_abs && !new_is_abs) {
        return yulafs_rename(old_rel, new_rel);
    }

    if (old_is_abs && new_is_abs) {
        char old_abs[128];
        char new_abs[128];

        if (vfs_build_abs_path(old_mountpoint, old_rel, old_abs, sizeof(old_abs)) != 0) {
            return -1;
        }

        if (vfs_build_abs_path(new_mountpoint, new_rel, new_abs, sizeof(new_abs)) != 0) {
            return -1;
        }

        return yulafs_rename(old_abs, new_abs);
    }

    if (old_is_abs) {
        char old_abs[128];
        if (vfs_build_abs_path(old_mountpoint, old_rel, old_abs, sizeof(old_abs)) != 0) {
            return -1;
        }

        return yulafs_rename(old_abs, new_rel);
    }

    char new_abs[128];
    if (vfs_build_abs_path(new_mountpoint, new_rel, new_abs, sizeof(new_abs)) != 0) {
        return -1;
    }

    return yulafs_rename(old_rel, new_abs);
}

static int vfs_yulafs_stat(const char* mountpoint, const char* rel, int is_abs, vfs_stat_t* out) {
    if (!rel || !out) {
        return -1;
    }

    const char* path = rel;
    char abs_path[128];
    if (is_abs) {
        if (vfs_build_abs_path(mountpoint, rel, abs_path, sizeof(abs_path)) != 0) {
            return -1;
        }

        path = abs_path;
    }

    const int inode_idx = yulafs_lookup(path);
    if (inode_idx < 0) {
        return -1;
    }

    yfs_inode_t info;
    if (yulafs_stat((yfs_ino_t)inode_idx, &info) != 0) {
        return -1;
    }

    out->type = info.type;
    out->size = info.size;
    return 0;
}

static int vfs_yulafs_get_fs_info(uint32_t* total_blocks, uint32_t* free_blocks, uint32_t* block_size) {
    if (!total_blocks || !free_blocks || !block_size) {
        return -1;
    }

    yulafs_get_filesystem_info(total_blocks, free_blocks, block_size);
    return 0;
}

static vfs_node_t* vfs_yulafs_create_node_from_path(const char* mountpoint, const char* rel, int is_abs) {
    if (!rel) {
        return nullptr;
    }

    const char* path = rel;
    char abs_path[128];
    if (is_abs) {
        if (vfs_build_abs_path(mountpoint, rel, abs_path, sizeof(abs_path)) != 0) {
            return nullptr;
        }

        path = abs_path;
    }

    const int inode = yulafs_lookup(path);
    if (inode == -1) {
        return nullptr;
    }

    vfs_node_t* node = (vfs_node_t*)kmalloc(sizeof(vfs_node_t));
    if (!node) {
        return nullptr;
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

static vfs_node_t* vfs_devfs_open(task_t* curr, const char* mountpoint, const char* rel, int is_abs, int flags) {
    (void)mountpoint;
    (void)is_abs;
    (void)flags;

    if (!rel || rel[0] == '\0') {
        return vfs_open_devfs_root_node();
    }

    return vfs_open_devfs_node(curr, rel);
}

static int vfs_devfs_stat(const char* mountpoint, const char* rel, int is_abs, vfs_stat_t* out) {
    (void)mountpoint;
    (void)is_abs;

    if (!out) {
        return -1;
    }

    if (!rel || rel[0] == '\0') {
        out->type = YFS_TYPE_DIR;
        out->size = 0;
        return 0;
    }

    vfs_node_t* tmpl = devfs_fetch(rel);
    if (!tmpl) {
        return -1;
    }

    out->type = YFS_TYPE_FILE;
    out->size = tmpl->size;
    return 0;
}

static int vfs_devfs_getdents(
    task_t* curr,
    vfs_node_t* dir_node,
    uint32_t* inout_offset,
    yfs_dirent_info_t* out,
    uint32_t out_size
) {
    (void)curr;

    if (!dir_node || (dir_node->flags & VFS_FLAG_DEVFS_ROOT) == 0) {
        return -1;
    }

    return devfs_registry().getdents(inout_offset, out, out_size);
}

static int vfs_devfs_fstatat(task_t* curr, vfs_node_t* dir_node, const char* name, vfs_stat_t* out) {
    if (!curr || !dir_node || !name || !*name || !out) {
        return -1;
    }

    if ((dir_node->flags & VFS_FLAG_DEVFS_ROOT) == 0) {
        return -1;
    }

    if (strcmp(name, "tty") == 0) {
        vfs_node_t* tty_node = curr->controlling_tty;
        if (!tty_node) {
            return -1;
        }

        out->type = YFS_TYPE_FILE;
        out->size = tty_node->size;
        return 0;
    }

    vfs_node_t* tmpl = devfs_fetch(name);
    if (!tmpl) {
        return -1;
    }

    out->type = YFS_TYPE_FILE;
    out->size = tmpl->size;
    return 0;
}

static vfs_node_t* vfs_devfs_create_node_from_path(const char* mountpoint, const char* rel, int is_abs) {
    (void)mountpoint;
    (void)is_abs;

    if (!rel || rel[0] == '\0') {
        return nullptr;
    }

    return devfs_clone(rel);
}

static vfs_node_t* vfs_open_yulafs_node(const char* path, int open_write, int open_append) {
    int inode = yulafs_lookup(path);

    if (inode == -1 && open_write) {
        inode = yulafs_create(path);
    }

    if (inode == -1) {
        return nullptr;
    }

    if (open_write && !open_append) {
        yfs_inode_t info;
        yulafs_stat((yfs_ino_t)inode, &info);

        if (info.type == YFS_TYPE_FILE) {
            yulafs_resize(inode, 0);
        }
    }

    auto* node = static_cast<vfs_node_t*>(kmalloc(sizeof(vfs_node_t)));
    if (!node) {
        return nullptr;
    }

    memset(node, 0, sizeof(vfs_node_t));
    node->inode_idx = inode;
    node->ops = &yfs_vfs_ops;
    node->flags = VFS_FLAG_YULAFS;

    yfs_inode_t info;
    if (yulafs_stat(inode, &info) == 0) {
        node->size = info.size;
    }

    strlcpy(node->name, path, sizeof(node->name));

    node->refs = 1;
    return node;
}

int vfs_open(const char* path, int flags) {
    task_t* curr = proc_current();
    if (!curr) {
        return -1;
    }

    if ((flags & ~3) != 0) {
        return -1;
    }

    const int open_append = (flags & 2) != 0;

    const vfs_mount_entry* mount = nullptr;
    const char* rel = nullptr;
    int is_abs = 0;
    if (vfs_resolve_mount(path, &mount, &rel, &is_abs) != 0 || !mount || !mount->driver || !mount->driver->ops) {
        return -1;
    }

    if (!mount->driver->ops->open) {
        return -1;
    }

    vfs_node_t* node = mount->driver->ops->open(curr, mount->mountpoint, rel, is_abs, flags);

    if (!node) {
        return -1;
    }

    node->refs = 1;

    if (node->ops && node->ops->open) {
        const int rc = node->ops->open(node);
        if (rc != 0) {
            vfs_node_release(node);
            return -1;
        }
    }

    file_desc_t* d = nullptr;
    const int fd = proc_fd_alloc(curr, &d);
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

    file_desc_t* d = nullptr;
    if (proc_fd_remove(curr, fd, &d) < 0 || !d) {
        return -1;
    }

    file_desc_release(d);
    return 0;
}

vfs_node_t* vfs_create_node_from_path(const char* path) {
    if (!path || path[0] == '\0') {
        return 0;
    }

    const vfs_mount_entry* mount = nullptr;
    const char* rel = nullptr;
    int is_abs = 0;
    if (vfs_resolve_mount(path, &mount, &rel, &is_abs) != 0 || !mount || !mount->driver || !mount->driver->ops) {
        return 0;
    }

    if (!mount->driver->ops->create_node_from_path) {
        return 0;
    }

    return mount->driver->ops->create_node_from_path(mount->mountpoint, rel, is_abs);
}

int vfs_mkdir(const char* path) {
    if (!path || path[0] == '\0') {
        return -1;
    }

    const vfs_mount_entry* mount = nullptr;
    const char* rel = nullptr;
    int is_abs = 0;
    if (vfs_resolve_mount(path, &mount, &rel, &is_abs) != 0 || !mount || !mount->driver || !mount->driver->ops) {
        return -1;
    }

    if (!mount->driver->ops->mkdir) {
        return -1;
    }

    return mount->driver->ops->mkdir(mount->mountpoint, rel, is_abs);
}

int vfs_unlink(const char* path) {
    if (!path || path[0] == '\0') {
        return -1;
    }

    const vfs_mount_entry* mount = nullptr;
    const char* rel = nullptr;
    int is_abs = 0;
    if (vfs_resolve_mount(path, &mount, &rel, &is_abs) != 0 || !mount || !mount->driver || !mount->driver->ops) {
        return -1;
    }

    if (!mount->driver->ops->unlink) {
        return -1;
    }

    return mount->driver->ops->unlink(mount->mountpoint, rel, is_abs);
}

int vfs_stat_path(const char* path, vfs_stat_t* out) {
    if (!path || path[0] == '\0' || !out) {
        return -1;
    }

    const vfs_mount_entry* mount = nullptr;
    const char* rel = nullptr;
    int is_abs = 0;
    if (vfs_resolve_mount(path, &mount, &rel, &is_abs) != 0 || !mount || !mount->driver || !mount->driver->ops) {
        return -1;
    }

    if (!mount->driver->ops->stat) {
        return -1;
    }

    return mount->driver->ops->stat(mount->mountpoint, rel, is_abs, out);
}

int vfs_rename(const char* old_path, const char* new_path) {
    if (!old_path || old_path[0] == '\0' || !new_path || new_path[0] == '\0') {
        return -1;
    }

    const vfs_mount_entry* old_mount = nullptr;
    const vfs_mount_entry* new_mount = nullptr;
    const char* old_rel = nullptr;
    const char* new_rel = nullptr;
    int old_is_abs = 0;
    int new_is_abs = 0;

    if (vfs_resolve_mount(old_path, &old_mount, &old_rel, &old_is_abs) != 0 || !old_mount || !old_mount->driver || !old_mount->driver->ops) {
        return -1;
    }

    if (vfs_resolve_mount(new_path, &new_mount, &new_rel, &new_is_abs) != 0 || !new_mount || !new_mount->driver || !new_mount->driver->ops) {
        return -1;
    }

    if (old_mount->driver != new_mount->driver) {
        return -1;
    }

    if (!old_mount->driver->ops->rename) {
        return -1;
    }

    return old_mount->driver->ops->rename(
        old_mount->mountpoint,
        old_rel,
        new_mount->mountpoint,
        new_rel,
        old_is_abs,
        new_is_abs
    );
}

int vfs_get_fs_info(uint32_t* total_blocks, uint32_t* free_blocks, uint32_t* block_size) {
    if (!total_blocks || !free_blocks || !block_size) {
        return -1;
    }

    const vfs_fs_driver* driver = vfs_driver_from_name("yulafs");
    if (!driver || !driver->ops || !driver->ops->get_fs_info) {
        return -1;
    }

    return driver->ops->get_fs_info(total_blocks, free_blocks, block_size);
}

} // extern "C"
