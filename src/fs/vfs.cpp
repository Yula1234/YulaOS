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

    if ((node->flags & VFS_FLAG_DEVFS_ROOT) != 0) {
        kernel::SpinLockNativeSafeGuard guard(d.get()->lock);
        return devfs_registry().getdents(&d.get()->offset, (yfs_dirent_info_t*)buf, size);
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

    if ((node->flags & VFS_FLAG_DEVFS_ROOT) != 0) {
        vfs_stat_t* st = (vfs_stat_t*)stat_buf;

        if (strcmp(name, "tty") == 0) {
            vfs_node_t* tty_node = curr ? curr->controlling_tty : nullptr;
            if (!tty_node) {
                return -1;
            }

            st->type = YFS_TYPE_FILE;
            st->size = tty_node->size;
            return 0;
        }

        vfs_node_t* tmpl = devfs_fetch(name);
        if (!tmpl) {
            return -1;
        }

        st->type = YFS_TYPE_FILE;
        st->size = tmpl->size;
        return 0;
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

static const char* vfs_normalize_abs_path(const char* path) {
    if (!path || path[0] == '\0') {
        return "";
    }

    if (path[0] == '/' && path[1] != '\0') {
        return path + 1;
    }

    if (path[0] == '/' && path[1] == '\0') {
        return "";
    }

    return path;
}

enum class vfs_mount_kind {
    yulafs,
    devfs,
};

struct vfs_mount_entry {
    vfs_mount_kind kind;
    char mountpoint[32];
    uint8_t used;
};

static vfs_mount_entry g_mounts[8];

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

static int vfs_mount_kind_from_name(const char* fs_name, vfs_mount_kind* out) {
    if (!fs_name || !out) {
        return -1;
    }

    if (strcmp(fs_name, "yulafs") == 0) {
        *out = vfs_mount_kind::yulafs;
        return 0;
    }

    if (strcmp(fs_name, "devfs") == 0) {
        *out = vfs_mount_kind::devfs;
        return 0;
    }

    return -1;
}

static int vfs_mount_table_insert(const char* mountpoint, vfs_mount_kind kind) {
    for (size_t i = 0; i < sizeof(g_mounts) / sizeof(g_mounts[0]); i++) {
        if (!g_mounts[i].used) {
            memset(&g_mounts[i], 0, sizeof(g_mounts[i]));
            g_mounts[i].used = 1u;
            g_mounts[i].kind = kind;
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

static int vfs_mount_table_find(const char* mountpoint, vfs_mount_kind* out_kind) {
    if (!mountpoint) {
        return -1;
    }

    for (size_t i = 0; i < sizeof(g_mounts) / sizeof(g_mounts[0]); i++) {
        if (!g_mounts[i].used) {
            continue;
        }

        if (strcmp(g_mounts[i].mountpoint, mountpoint) == 0) {
            if (out_kind) {
                *out_kind = g_mounts[i].kind;
            }
            return 0;
        }
    }

    return -1;
}

static int vfs_resolve_mount(const char* path, vfs_mount_kind* out_kind, const char** out_rel) {
    if (!path || path[0] == '\0' || !out_kind || !out_rel) {
        return -1;
    }

    if (!vfs_path_is_abs(path)) {
        return -1;
    }

    int best_len = -1;
    vfs_mount_kind best_kind = vfs_mount_kind::yulafs;
    const char* best_mount = "/";

    for (size_t i = 0; i < sizeof(g_mounts) / sizeof(g_mounts[0]); i++) {
        if (!g_mounts[i].used) {
            continue;
        }

        const int mlen = vfs_mount_match_len(path, g_mounts[i].mountpoint);
        if (mlen > best_len) {
            best_len = mlen;
            best_kind = g_mounts[i].kind;
            best_mount = g_mounts[i].mountpoint;
        }
    }

    if (best_len < 0 || !best_mount) {
        return -1;
    }

    const size_t mlen = strlen(best_mount);
    const char* rel = path + mlen;
    if (rel[0] == '/') {
        rel++;
    }

    *out_kind = best_kind;
    *out_rel = rel;
    return 0;
}

static int vfs_mount_impl(const char* mountpoint, const char* fs_name) {
    if (!mountpoint || !fs_name) {
        return -1;
    }

    if (!vfs_mountpoint_is_valid(mountpoint)) {
        return -1;
    }

    vfs_mount_kind existing;
    if (vfs_mount_table_find(mountpoint, &existing) == 0) {
        return -1;
    }

    vfs_mount_kind kind;
    if (vfs_mount_kind_from_name(fs_name, &kind) != 0) {
        return -1;
    }

    if (vfs_mount_table_insert(mountpoint, kind) != 0) {
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
    const int open_write = ((flags & 1) != 0) || open_append;

    vfs_node_t* node = nullptr;

    vfs_mount_kind kind;
    const char* rel = nullptr;
    if (vfs_resolve_mount(path, &kind, &rel) != 0) {
        return -1;
    }

    if (kind == vfs_mount_kind::devfs) {
        if (!rel || rel[0] == '\0') {
            node = vfs_open_devfs_root_node();
        } else {
            node = vfs_open_devfs_node(curr, rel);
        }
    } else {
        node = vfs_open_yulafs_node(path, open_write, open_append);
    }

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

    vfs_mount_kind kind;
    const char* rel = nullptr;
    if (vfs_resolve_mount(path, &kind, &rel) != 0) {
        return 0;
    }

    if (kind == vfs_mount_kind::devfs) {
        if (!rel || rel[0] == '\0') {
            return 0;
        }

        return devfs_clone(rel);
    }

    const int inode = yulafs_lookup(path);
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

static int vfs_is_dev_path(const char* path) {
    vfs_mount_kind kind;
    const char* rel = nullptr;
    if (vfs_resolve_mount(path, &kind, &rel) != 0) {
        return 0;
    }

    return kind == vfs_mount_kind::devfs;
}

int vfs_mkdir(const char* path) {
    if (!path || path[0] == '\0') {
        return -1;
    }

    if (vfs_is_dev_path(path)) {
        return -1;
    }

    return yulafs_mkdir(path);
}

int vfs_unlink(const char* path) {
    if (!path || path[0] == '\0') {
        return -1;
    }

    if (vfs_is_dev_path(path)) {
        return -1;
    }

    return yulafs_unlink(path);
}

int vfs_stat_path(const char* path, vfs_stat_t* out) {
    if (!path || path[0] == '\0' || !out) {
        return -1;
    }

    vfs_mount_kind kind;
    const char* rel = nullptr;
    if (vfs_resolve_mount(path, &kind, &rel) != 0) {
        return -1;
    }

    if (kind == vfs_mount_kind::devfs) {
        if (!rel || rel[0] == '\0') {
            return -1;
        }

        vfs_node_t* tmpl = devfs_fetch(rel);
        if (!tmpl) {
            return -1;
        }

        out->type = YFS_TYPE_FILE;
        out->size = tmpl->size;
        return 0;
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

int vfs_rename(const char* old_path, const char* new_path) {
    if (!old_path || old_path[0] == '\0' || !new_path || new_path[0] == '\0') {
        return -1;
    }

    if (vfs_is_dev_path(old_path) || vfs_is_dev_path(new_path)) {
        return -1;
    }

    return yulafs_rename(old_path, new_path);
}

int vfs_get_fs_info(uint32_t* total_blocks, uint32_t* free_blocks, uint32_t* block_size) {
    if (!total_blocks || !free_blocks || !block_size) {
        return -1;
    }

    yulafs_get_filesystem_info(total_blocks, free_blocks, block_size);
    return 0;
}

} // extern "C"
