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

struct vfs_fs_instance;

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
    node->flags &= ~VFS_FLAG_INSTANCE_REF;
    node->fs_driver = snapshot.fs_driver;
    return node;
}

class DevFSRegistry {
public:
    void set_instance(vfs_fs_instance* inst) noexcept;
    void register_node(vfs_node_t* node) noexcept;
    int unregister_node(const char* name) noexcept;
    vfs_node_t* fetch_borrowed(const char* name) noexcept;
    vfs_node_t* clone(const char* name) noexcept;
    vfs_node_t* take(const char* name) noexcept;
    int getdents(uint32_t* inout_offset, yfs_dirent_info_t* out, uint32_t out_size) noexcept;

private:
    kernel::SpinLock lock_;
    HashMap<kernel::string, vfs_node_t*, 256> nodes_;
    vfs_fs_instance* instance_ = nullptr;
};

static DevFSRegistry g_devfs;

static DevFSRegistry& devfs_registry() noexcept {
    return g_devfs;
}

} // namespace


struct vfs_fs_instance;

struct vfs_fs_type_ops {
    vfs_node_t* (*open)(vfs_fs_instance* inst, task_t* curr, const char* mountpoint, const char* rel, int is_abs, int flags);
    int (*mkdir)(vfs_fs_instance* inst, const char* mountpoint, const char* rel, int is_abs);
    int (*unlink)(vfs_fs_instance* inst, const char* mountpoint, const char* rel, int is_abs);
    int (*rename)(
        vfs_fs_instance* inst,
        const char* old_mountpoint,
        const char* old_rel,
        const char* new_mountpoint,
        const char* new_rel,
        int old_is_abs,
        int new_is_abs
    );
    int (*stat)(vfs_fs_instance* inst, const char* mountpoint, const char* rel, int is_abs, vfs_stat_t* out);
    int (*getdents)(vfs_fs_instance* inst, task_t* curr, vfs_node_t* dir_node, uint32_t* inout_offset, yfs_dirent_info_t* out, uint32_t out_size);
    int (*fstatat)(vfs_fs_instance* inst, task_t* curr, vfs_node_t* dir_node, const char* name, vfs_stat_t* out);
    int (*append)(vfs_fs_instance* inst, task_t* curr, vfs_node_t* node, const void* buf, uint32_t size, uint32_t* out_new_offset);
    int (*get_fs_info)(vfs_fs_instance* inst, uint32_t* total_blocks, uint32_t* free_blocks, uint32_t* block_size);
    vfs_node_t* (*create_node_from_path)(vfs_fs_instance* inst, const char* mountpoint, const char* rel, int is_abs);
};

struct vfs_fs_type;

struct vfs_fs_instance {
    const vfs_fs_type* type;
    void* fs_private;
    vfs_node_t* root;
    uint32_t refs;
};

static void vfs_instance_retain(vfs_fs_instance* inst) noexcept {
    if (!inst) {
        return;
    }

    __sync_fetch_and_add(&inst->refs, 1);
}

static void vfs_instance_release(vfs_fs_instance* inst) noexcept {
    if (!inst) {
        return;
    }

    __sync_sub_and_fetch(&inst->refs, 1);
}

static void vfs_node_bind_instance(vfs_node_t* node, vfs_fs_instance* inst) noexcept {
    if (!node || !inst) {
        return;
    }

    node->fs_driver = inst;

    if ((node->flags & VFS_FLAG_INSTANCE_REF) == 0u) {
        vfs_instance_retain(inst);
        node->flags |= VFS_FLAG_INSTANCE_REF;
    }
}

struct vfs_fs_type {
    const char* name;
    const vfs_fs_type_ops* ops;
    vfs_fs_instance* (*mount)(const char* source, uint32_t flags);
    int (*umount)(vfs_fs_instance* inst);
};

struct vfs_mount_entry {
    vfs_fs_instance* instance;
    char mountpoint[32];
    uint8_t used;
};

extern const vfs_fs_type g_yulafs_type;
extern const vfs_fs_type g_devfs_type;

static vfs_node_t* vfs_open_yulafs_node(const char* path, int open_write, int open_append);
static vfs_node_t* vfs_open_devfs_root_node(void);

static vfs_fs_instance* vfs_yulafs_mount(const char* source, uint32_t flags) {
    (void)source;
    (void)flags;

    auto* inst = static_cast<vfs_fs_instance*>(kmalloc(sizeof(vfs_fs_instance)));
    if (!inst) {
        return nullptr;
    }

    memset(inst, 0, sizeof(*inst));
    inst->type = &g_yulafs_type;
    inst->refs = 1;

    vfs_node_t* root = vfs_open_yulafs_node("/", 0, 0);
    if (!root) {
        kfree(inst);
        return nullptr;
    }

    root->fs_driver = inst;
    inst->root = root;

    return inst;
}

static int vfs_yulafs_umount(vfs_fs_instance* inst) {
    if (!inst || inst->type != &g_yulafs_type) {
        return -1;
    }

    if (inst->root) {
        vfs_node_release(inst->root);
        inst->root = nullptr;
    }

    kfree(inst);
    return 0;
}

static vfs_fs_instance* vfs_devfs_mount(const char* source, uint32_t flags) {
    (void)source;
    (void)flags;

    auto* inst = static_cast<vfs_fs_instance*>(kmalloc(sizeof(vfs_fs_instance)));
    if (!inst) {
        return nullptr;
    }

    memset(inst, 0, sizeof(*inst));
    inst->type = &g_devfs_type;
    inst->refs = 1;

    vfs_node_t* root = vfs_open_devfs_root_node();
    if (!root) {
        kfree(inst);
        return nullptr;
    }

    root->fs_driver = inst;
    inst->root = root;

    devfs_registry().set_instance(inst);
    return inst;
}

static int vfs_devfs_umount(vfs_fs_instance* inst) {
    if (!inst || inst->type != &g_devfs_type) {
        return -1;
    }

    devfs_registry().set_instance(nullptr);

    if (inst->root) {
        vfs_node_release(inst->root);
        inst->root = nullptr;
    }

    kfree(inst);
    return 0;
}

namespace {

void DevFSRegistry::set_instance(vfs_fs_instance* inst) noexcept {
    kernel::SpinLockSafeGuard guard(lock_);
    instance_ = inst;
}

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
    node->flags &= ~VFS_FLAG_INSTANCE_REF;
    node->fs_driver = snapshot.fs_driver;

    if (!node->fs_driver) {
        kernel::SpinLockSafeGuard guard(lock_);
        node->fs_driver = instance_;
    }

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

        dst->inode = (uint32_t)(((uintptr_t)tmpl >> 4) | 1u);
        dst->type = YFS_TYPE_FILE;
        dst->size = tmpl->size;
        strlcpy(dst->name, tmpl->name, sizeof(dst->name));

        written++;
        (*inout_offset)++;
    }

    return (int)(written * (uint32_t)sizeof(yfs_dirent_info_t));
}

} // namespace

extern "C" void devfs_register(vfs_node_t* node) {
    devfs_registry().register_node(node);
}

extern "C" int devfs_unregister(const char* name) {
    return devfs_registry().unregister_node(name);
}

extern "C" vfs_node_t* devfs_fetch(const char* name) {
    return devfs_registry().fetch_borrowed(name);
}

extern "C" vfs_node_t* devfs_clone(const char* name) {
    return devfs_registry().clone(name);
}

extern "C" vfs_node_t* devfs_take(const char* name) {
    return devfs_registry().take(name);
}

static void vfs_init_impl(void);
static int vfs_mount_impl(const char* mountpoint, const char* fs_name);
static int vfs_umount_impl(const char* mountpoint);

extern "C" void vfs_init(void) {
    vfs_init_impl();
}

extern "C" int vfs_mount(const char* mountpoint, const char* fs_name) {
    return vfs_mount_impl(mountpoint, fs_name);
}

extern "C" int vfs_umount(const char* mountpoint) {
    return vfs_umount_impl(mountpoint);
}

extern "C" void vfs_node_retain(vfs_node_t* node) {
    if (!node) {
        return;
    }

    __sync_fetch_and_add(&node->refs, 1);
}

extern "C" void vfs_node_release(vfs_node_t* node) {
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

    if ((node->flags & VFS_FLAG_INSTANCE_REF) != 0u) {
        vfs_instance_release((vfs_fs_instance*)node->fs_driver);
        node->flags &= ~VFS_FLAG_INSTANCE_REF;
    }

    kfree(node);
}

extern "C" int vfs_getdents(int fd, void* buf, uint32_t size) {
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

    vfs_fs_instance* inst = (vfs_fs_instance*)node->fs_driver;
    if (!inst || !inst->type || !inst->type->ops || !inst->type->ops->getdents) {
        return -1;
    }

    kernel::SpinLockNativeSafeGuard guard(d.get()->lock);
    return inst->type->ops->getdents(inst, curr, node, &d.get()->offset, (yfs_dirent_info_t*)buf, size);
}

extern "C" int vfs_fstatat(int dirfd, const char* name, void* stat_buf) {
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

    vfs_fs_instance* inst = (vfs_fs_instance*)node->fs_driver;
    if (!inst || !inst->type || !inst->type->ops || !inst->type->ops->fstatat) {
        return -1;
    }

    return inst->type->ops->fstatat(inst, curr, node, name, (vfs_stat_t*)stat_buf);
}

static int vfs_yulafs_append(vfs_fs_instance* inst, task_t* curr, vfs_node_t* node, const void* buf, uint32_t size, uint32_t* out_new_offset) {
    (void)inst;
    (void)curr;

    if (!node || !buf || size == 0 || !out_new_offset) {
        return -1;
    }

    yfs_off_t start = 0;
    const int res = yulafs_append(node->inode_idx, buf, size, &start);
    if (res > 0) {
        *out_new_offset = (uint32_t)start + (uint32_t)res;
    }

    return res;
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

static kernel::SpinLock g_mount_lock;
static HashMap<kernel::string, vfs_mount_entry*, 32> g_mounts;

static const vfs_fs_type* vfs_fs_type_from_name(const char* fs_name);

static int vfs_mountpoint_is_valid(const char* mountpoint) {
    if (!mountpoint || mountpoint[0] != '/') {
        return 0;
    }

    if (mountpoint[1] == '\0') {
        return 1;
    }

    const size_t n = strlen(mountpoint);
    if (n == 0 || n >= sizeof(((vfs_mount_entry*)nullptr)->mountpoint)) {
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

static constexpr size_t VFS_PATH_MAX = 256;
static constexpr uint32_t VFS_PATH_SEG_MAX = 128;

struct VfsPathSegment {
    const char* ptr;
    uint16_t len;
};

static int vfs_collect_segments(const char* path, VfsPathSegment* segments, uint32_t* count, uint32_t cap) {
    if (!path || !segments || !count) {
        return -1;
    }

    const char* p = path;
    while (*p) {
        while (*p == '/') {
            p++;
        }
        if (*p == '\0') {
            break;
        }

        const char* start = p;
        while (*p && *p != '/') {
            p++;
        }

        const size_t len = (size_t)(p - start);
        if (len == 0) {
            continue;
        }

        if (len == 1 && start[0] == '.') {
            continue;
        }

        if (len == 2 && start[0] == '.' && start[1] == '.') {
            if (*count > 0) {
                (*count)--;
            }
            continue;
        }

        if (*count >= cap) {
            return -1;
        }

        segments[*count].ptr = start;
        segments[*count].len = (uint16_t)len;
        (*count)++;
    }

    return 0;
}

static int vfs_normalize_path(task_t* curr, const char* path, char* out, size_t out_size) {
    if (!curr || !path || path[0] == '\0' || !out || out_size < 2u) {
        return -1;
    }

    VfsPathSegment segments[VFS_PATH_SEG_MAX];
    uint32_t count = 0;

    if (path[0] != '/') {
        char cwd[VFS_PATH_MAX];
        const yfs_ino_t cwd_ino = (yfs_ino_t)(curr->cwd_inode ? curr->cwd_inode : 1u);
        if (yulafs_inode_to_path(cwd_ino, cwd, (uint32_t)sizeof(cwd)) < 0) {
            return -1;
        }

        if (vfs_collect_segments(cwd, segments, &count, VFS_PATH_SEG_MAX) != 0) {
            return -1;
        }
    }

    if (vfs_collect_segments(path, segments, &count, VFS_PATH_SEG_MAX) != 0) {
        return -1;
    }

    size_t pos = 0;
    out[pos++] = '/';

    for (uint32_t i = 0; i < count; i++) {
        if (pos > 1) {
            if (pos + 1 >= out_size) {
                return -1;
            }
            out[pos++] = '/';
        }

        if (pos + segments[i].len >= out_size) {
            return -1;
        }

        memcpy(out + pos, segments[i].ptr, segments[i].len);
        pos += segments[i].len;
    }

    out[pos] = '\0';
    return 0;
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

static vfs_mount_entry* vfs_mount_table_find_locked(const char* mountpoint) {
    if (!mountpoint) {
        return nullptr;
    }

    const kernel::string key(mountpoint);
    auto locked = g_mounts.find_ptr(key);
    if (!locked) {
        return nullptr;
    }

    vfs_mount_entry** entry_ptr = locked.value_ptr();
    return entry_ptr ? *entry_ptr : nullptr;
}

static int vfs_mount_table_insert_locked(const char* mountpoint, vfs_fs_instance* instance) {
    if (!mountpoint || !instance) {
        return -1;
    }

    if (vfs_mount_table_find_locked(mountpoint)) {
        return -1;
    }

    auto* entry = static_cast<vfs_mount_entry*>(kmalloc(sizeof(vfs_mount_entry)));
    if (!entry) {
        return -1;
    }

    memset(entry, 0, sizeof(*entry));
    entry->used = 1u;
    entry->instance = instance;
    strlcpy(entry->mountpoint, mountpoint, sizeof(entry->mountpoint));

    const kernel::string key(mountpoint);
    g_mounts.insert_or_assign(key, entry);
    return 0;
}

static int vfs_mount_table_remove_locked(const char* mountpoint, vfs_mount_entry** out_entry) {
    if (!mountpoint || !out_entry) {
        return -1;
    }

    vfs_mount_entry* entry = vfs_mount_table_find_locked(mountpoint);
    if (!entry) {
        return -1;
    }

    const kernel::string key(mountpoint);
    if (!g_mounts.remove(key)) {
        return -1;
    }

    *out_entry = entry;
    return 0;
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

    kernel::SpinLockSafeGuard guard(g_mount_lock);

    if (!is_abs) {
        vfs_mount_entry* root = vfs_mount_table_find_locked("/");
        if (!root || !root->instance || !root->instance->type || !root->instance->type->ops) {
            return -1;
        }

        *out_mount = root;
        *out_rel = path;
        return 0;
    }

    const size_t max_mount_len = sizeof(((vfs_mount_entry*)nullptr)->mountpoint) - 1u;
    vfs_mount_entry* best = vfs_mount_table_find_locked("/");
    size_t best_len = best ? 1u : 0u;

    const size_t path_len = strlen(path);
    char prefix[sizeof(((vfs_mount_entry*)nullptr)->mountpoint)];

    for (size_t i = 1u; i < path_len; i++) {
        if (path[i] != '/') {
            continue;
        }

        const size_t len = i;
        if (len > max_mount_len) {
            break;
        }

        memcpy(prefix, path, len);
        prefix[len] = '\0';

        vfs_mount_entry* entry = vfs_mount_table_find_locked(prefix);
        if (entry && entry->instance && entry->instance->type && entry->instance->type->ops) {
            best = entry;
            best_len = len;
        }
    }

    if (path_len <= max_mount_len) {
        memcpy(prefix, path, path_len);
        prefix[path_len] = '\0';

        vfs_mount_entry* entry = vfs_mount_table_find_locked(prefix);
        if (entry && entry->instance && entry->instance->type && entry->instance->type->ops) {
            best = entry;
            best_len = path_len;
        }
    }

    if (!best || best_len == 0u) {
        return -1;
    }

    const char* rel = path + best_len;
    if (rel[0] == '/') {
        rel++;
    }

    *out_mount = best;
    *out_rel = rel;
    return 0;
}

struct VfsResolvedPath {
    const vfs_mount_entry* mount;
    const char* rel;
    int is_abs;
    char abs[VFS_PATH_MAX];
};

static int vfs_dirfd_to_base(task_t* curr, int dirfd, char* out, size_t out_size) {
    if (!curr || !out || out_size < 2u) {
        return -1;
    }

    if (dirfd < 0) {
        const yfs_ino_t cwd_ino = (yfs_ino_t)(curr->cwd_inode ? curr->cwd_inode : 1u);
        return yulafs_inode_to_path(cwd_ino, out, (uint32_t)out_size) < 0 ? -1 : 0;
    }

    FileDescHandle d(curr, dirfd);
    if (!d || !d.get()->node) {
        return -1;
    }

    vfs_node_t* node = d.get()->node;
    if ((node->flags & VFS_FLAG_DEVFS_ROOT) != 0u) {
        return strlcpy(out, "/dev", out_size) >= out_size ? -1 : 0;
    }

    if ((node->flags & VFS_FLAG_YULAFS) != 0u) {
        yfs_inode_t info;
        if (yulafs_stat((yfs_ino_t)node->inode_idx, &info) != 0 || info.type != YFS_TYPE_DIR) {
            return -1;
        }

        return yulafs_inode_to_path((yfs_ino_t)node->inode_idx, out, (uint32_t)out_size) < 0 ? -1 : 0;
    }

    return -1;
}

static int vfs_resolve_path_base(task_t* curr, const char* base, const char* path, VfsResolvedPath* out) {
    if (!curr || !path || !out) {
        return -1;
    }

    const char* src = path;
    char merged[VFS_PATH_MAX];

    if (path[0] != '/' && base && base[0] != '\0') {
        if (vfs_build_abs_path(base, path, merged, sizeof(merged)) != 0) {
            return -1;
        }
        src = merged;
    }

    if (vfs_normalize_path(curr, src, out->abs, sizeof(out->abs)) != 0) {
        return -1;
    }

    const vfs_mount_entry* mount = nullptr;
    const char* rel = nullptr;
    int is_abs = 0;
    if (vfs_resolve_mount(out->abs, &mount, &rel, &is_abs) != 0) {
        return -1;
    }

    out->mount = mount;
    out->rel = rel;
    out->is_abs = is_abs;
    return 0;
}

static int vfs_resolve_path(task_t* curr, const char* path, VfsResolvedPath* out) {
    return vfs_resolve_path_base(curr, nullptr, path, out);
}

static int vfs_yulafs_getdents(
    vfs_fs_instance* inst,
    task_t* curr,
    vfs_node_t* dir_node,
    uint32_t* inout_offset,
    yfs_dirent_info_t* out,
    uint32_t out_size
) {
    (void)inst;
    (void)curr;

    if (!dir_node || (dir_node->flags & VFS_FLAG_YULAFS) == 0) {
        return -1;
    }

    return yulafs_getdents(dir_node->inode_idx, inout_offset, out, out_size);
}

static int vfs_yulafs_fstatat(vfs_fs_instance* inst, task_t* curr, vfs_node_t* dir_node, const char* name, vfs_stat_t* out) {
    (void)inst;
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

    {
        kernel::SpinLockSafeGuard guard(g_mount_lock);
        if (vfs_mount_table_find_locked(mountpoint)) {
            return -1;
        }
    }

    const vfs_fs_type* type = vfs_fs_type_from_name(fs_name);
    if (!type || !type->ops || !type->mount || !type->umount) {
        return -1;
    }

    vfs_fs_instance* instance = type->mount(nullptr, 0u);
    if (!instance || instance->type != type || !instance->type || !instance->type->ops) {
        if (instance && type->umount) {
            (void)type->umount(instance);
        }

        return -1;
    }

    {
        kernel::SpinLockSafeGuard guard(g_mount_lock);
        if (vfs_mount_table_insert_locked(mountpoint, instance) != 0) {
            (void)type->umount(instance);
            return -1;
        }
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

    vfs_mount_entry* entry = nullptr;
    {
        kernel::SpinLockSafeGuard guard(g_mount_lock);
        if (vfs_mount_table_remove_locked(mountpoint, &entry) != 0 || !entry) {
            return -1;
        }
    }

    vfs_fs_instance* inst = entry->instance;
    if (!inst || !inst->type || !inst->type->umount) {
        kfree(entry);
        return -1;
    }

    if (inst->refs > 1u) {
        kfree(entry);
        return -1;
    }

    const int rc = inst->type->umount(inst);
    kfree(entry);
    return rc;
}

static void vfs_init_impl(void) {
    {
        kernel::SpinLockSafeGuard guard(g_mount_lock);
        g_mounts.clear();
    }

    (void)vfs_mount_impl("/", "yulafs");
    (void)vfs_mount_impl("/dev", "devfs");
}

static vfs_node_t* vfs_yulafs_open(vfs_fs_instance* inst, task_t* curr, const char* mountpoint, const char* rel, int is_abs, int flags);
static int vfs_yulafs_mkdir(vfs_fs_instance* inst, const char* mountpoint, const char* rel, int is_abs);
static int vfs_yulafs_unlink(vfs_fs_instance* inst, const char* mountpoint, const char* rel, int is_abs);
static int vfs_yulafs_rename(
    vfs_fs_instance* inst,
    const char* old_mountpoint,
    const char* old_rel,
    const char* new_mountpoint,
    const char* new_rel,
    int old_is_abs,
    int new_is_abs
);
static int vfs_yulafs_stat(vfs_fs_instance* inst, const char* mountpoint, const char* rel, int is_abs, vfs_stat_t* out);
static int vfs_yulafs_getdents(vfs_fs_instance* inst, task_t* curr, vfs_node_t* dir_node, uint32_t* inout_offset, yfs_dirent_info_t* out, uint32_t out_size);
static int vfs_yulafs_fstatat(vfs_fs_instance* inst, task_t* curr, vfs_node_t* dir_node, const char* name, vfs_stat_t* out);
static int vfs_yulafs_get_fs_info(vfs_fs_instance* inst, uint32_t* total_blocks, uint32_t* free_blocks, uint32_t* block_size);
static vfs_node_t* vfs_yulafs_create_node_from_path(vfs_fs_instance* inst, const char* mountpoint, const char* rel, int is_abs);

static vfs_node_t* vfs_devfs_open(vfs_fs_instance* inst, task_t* curr, const char* mountpoint, const char* rel, int is_abs, int flags);
static int vfs_devfs_stat(vfs_fs_instance* inst, const char* mountpoint, const char* rel, int is_abs, vfs_stat_t* out);
static int vfs_devfs_getdents(vfs_fs_instance* inst, task_t* curr, vfs_node_t* dir_node, uint32_t* inout_offset, yfs_dirent_info_t* out, uint32_t out_size);
static int vfs_devfs_fstatat(vfs_fs_instance* inst, task_t* curr, vfs_node_t* dir_node, const char* name, vfs_stat_t* out);
static vfs_node_t* vfs_devfs_create_node_from_path(vfs_fs_instance* inst, const char* mountpoint, const char* rel, int is_abs);

static vfs_fs_instance* vfs_yulafs_mount(const char* source, uint32_t flags);
static int vfs_yulafs_umount(vfs_fs_instance* inst);
static vfs_fs_instance* vfs_devfs_mount(const char* source, uint32_t flags);
static int vfs_devfs_umount(vfs_fs_instance* inst);

static const vfs_fs_type_ops g_yulafs_type_ops = {
    vfs_yulafs_open,
    vfs_yulafs_mkdir,
    vfs_yulafs_unlink,
    vfs_yulafs_rename,
    vfs_yulafs_stat,
    vfs_yulafs_getdents,
    vfs_yulafs_fstatat,
    vfs_yulafs_append,
    vfs_yulafs_get_fs_info,
    vfs_yulafs_create_node_from_path,
};

const vfs_fs_type g_yulafs_type = {
    "yulafs",
    &g_yulafs_type_ops,
    vfs_yulafs_mount,
    vfs_yulafs_umount,
};

static const vfs_fs_type_ops g_devfs_type_ops = {
    vfs_devfs_open,
    0,
    0,
    0,
    vfs_devfs_stat,
    vfs_devfs_getdents,
    vfs_devfs_fstatat,
    0,
    0,
    vfs_devfs_create_node_from_path,
};

const vfs_fs_type g_devfs_type = {
    "devfs",
    &g_devfs_type_ops,
    vfs_devfs_mount,
    vfs_devfs_umount,
};

static const vfs_fs_type* vfs_fs_type_from_name(const char* fs_name) {
    if (!fs_name) {
        return nullptr;
    }

    if (strcmp(fs_name, g_yulafs_type.name) == 0) {
        return &g_yulafs_type;
    }

    if (strcmp(fs_name, g_devfs_type.name) == 0) {
        return &g_devfs_type;
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

static vfs_node_t* vfs_yulafs_open(vfs_fs_instance* inst, task_t* curr, const char* mountpoint, const char* rel, int is_abs, int flags) {
    (void)curr;

    const int open_append = (flags & 2) != 0;
    const int open_write = ((flags & 1) != 0) || open_append;

    if (!is_abs) {
        vfs_node_t* node = vfs_open_yulafs_node(rel, open_write, open_append);
        if (node) {
            node->fs_driver = inst;
        }

        return node;
    }

    char abs_path[128];
    if (vfs_build_abs_path(mountpoint, rel, abs_path, sizeof(abs_path)) != 0) {
        return nullptr;
    }

    vfs_node_t* node = vfs_open_yulafs_node(abs_path, open_write, open_append);
    if (node) {
        node->fs_driver = inst;
    }

    return node;
}

static int vfs_yulafs_mkdir(vfs_fs_instance* inst, const char* mountpoint, const char* rel, int is_abs) {
    (void)inst;
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

static int vfs_yulafs_unlink(vfs_fs_instance* inst, const char* mountpoint, const char* rel, int is_abs) {
    (void)inst;
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
    vfs_fs_instance* inst,
    const char* old_mountpoint,
    const char* old_rel,
    const char* new_mountpoint,
    const char* new_rel,
    int old_is_abs,
    int new_is_abs
) {
    (void)inst;
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

static int vfs_yulafs_stat(vfs_fs_instance* inst, const char* mountpoint, const char* rel, int is_abs, vfs_stat_t* out) {
    (void)inst;
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

static int vfs_yulafs_get_fs_info(vfs_fs_instance* inst, uint32_t* total_blocks, uint32_t* free_blocks, uint32_t* block_size) {
    (void)inst;
    if (!total_blocks || !free_blocks || !block_size) {
        return -1;
    }

    yulafs_get_filesystem_info(total_blocks, free_blocks, block_size);
    return 0;
}

static vfs_node_t* vfs_yulafs_create_node_from_path(vfs_fs_instance* inst, const char* mountpoint, const char* rel, int is_abs) {
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
    node->fs_driver = inst;

    yfs_inode_t info;
    if (yulafs_stat(inode, &info) == 0) {
        node->size = info.size;
    }

    strlcpy(node->name, path, sizeof(node->name));

    node->refs = 1;
    return node;
}

static vfs_node_t* vfs_devfs_open(vfs_fs_instance* inst, task_t* curr, const char* mountpoint, const char* rel, int is_abs, int flags) {
    (void)inst;
    (void)mountpoint;
    (void)is_abs;
    (void)flags;

    if (!rel || rel[0] == '\0') {
        if (!inst || !inst->root) {
            return nullptr;
        }

        return vfs_node_clone_existing(inst->root);
    }

    vfs_node_t* node = vfs_open_devfs_node(curr, rel);
    if (node) {
        node->fs_driver = inst;
    }

    return node;
}

static int vfs_devfs_stat(vfs_fs_instance* inst, const char* mountpoint, const char* rel, int is_abs, vfs_stat_t* out) {
    (void)inst;
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
    vfs_fs_instance* inst,
    task_t* curr,
    vfs_node_t* dir_node,
    uint32_t* inout_offset,
    yfs_dirent_info_t* out,
    uint32_t out_size
) {
    (void)inst;
    (void)curr;

    if (!dir_node || (dir_node->flags & VFS_FLAG_DEVFS_ROOT) == 0) {
        return -1;
    }

    return devfs_registry().getdents(inout_offset, out, out_size);
}

static int vfs_devfs_fstatat(vfs_fs_instance* inst, task_t* curr, vfs_node_t* dir_node, const char* name, vfs_stat_t* out) {
    (void)inst;
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

static vfs_node_t* vfs_devfs_create_node_from_path(vfs_fs_instance* inst, const char* mountpoint, const char* rel, int is_abs) {
    (void)mountpoint;
    (void)is_abs;

    if (!rel || rel[0] == '\0') {
        return nullptr;
    }

    vfs_node_t* node = devfs_clone(rel);
    if (node) {
        node->fs_driver = inst;
    }

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

static int vfs_open_resolved(task_t* curr, const VfsResolvedPath& resolved, int flags) {
    if (!curr || !resolved.mount || !resolved.mount->instance || !resolved.mount->instance->type || !resolved.mount->instance->type->ops) {
        return -1;
    }

    if (!resolved.mount->instance->type->ops->open) {
        return -1;
    }

    vfs_node_t* node = resolved.mount->instance->type->ops->open(
        resolved.mount->instance,
        curr,
        resolved.mount->mountpoint,
        resolved.rel,
        resolved.is_abs,
        flags
    );

    if (!node) {
        return -1;
    }

    vfs_node_bind_instance(node, resolved.mount->instance);

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

    const int open_append = (flags & 2) != 0;

    d->node = node;
    d->offset = 0;
    d->flags = open_append ? FILE_FLAG_APPEND : 0u;

    return fd;
}

extern "C" int vfs_open(const char* path, int flags) {
    task_t* curr = proc_current();
    if (!curr) {
        return -1;
    }

    if ((flags & ~3) != 0) {
        return -1;
    }

    VfsResolvedPath resolved;
    if (vfs_resolve_path(curr, path, &resolved) != 0 || !resolved.mount || !resolved.mount->instance || !resolved.mount->instance->type || !resolved.mount->instance->type->ops) {
        return -1;
    }

    return vfs_open_resolved(curr, resolved, flags);
}

extern "C" int vfs_openat(int dirfd, const char* path, int flags) {
    task_t* curr = proc_current();
    if (!curr || !path || path[0] == '\0') {
        return -1;
    }

    if ((flags & ~3) != 0) {
        return -1;
    }

    char base[VFS_PATH_MAX];
    if (vfs_dirfd_to_base(curr, dirfd, base, sizeof(base)) != 0) {
        return -1;
    }

    VfsResolvedPath resolved;
    if (vfs_resolve_path_base(curr, base, path, &resolved) != 0 || !resolved.mount || !resolved.mount->instance || !resolved.mount->instance->type || !resolved.mount->instance->type->ops) {
        return -1;
    }

    return vfs_open_resolved(curr, resolved, flags);
}

extern "C" int vfs_read(int fd, void* buf, uint32_t size) {
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

extern "C" int vfs_write(int fd, const void* buf, uint32_t size) {
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

    if ((fflags & FILE_FLAG_APPEND) != 0) {
        vfs_fs_instance* inst = (vfs_fs_instance*)d.get()->node->fs_driver;
        if (inst && inst->type && inst->type->ops && inst->type->ops->append) {
            uint32_t new_offset = 0;
            const int res = inst->type->ops->append(inst, curr, d.get()->node, buf, size, &new_offset);
            if (res > 0) {
                kernel::SpinLockNativeSafeGuard guard(d.get()->lock);
                d.get()->offset = new_offset;
            }

            return res;
        }
    }

    const int res = d.get()->node->ops->write(d.get()->node, off, size, buf);
    if (res > 0) {
        kernel::SpinLockNativeSafeGuard guard(d.get()->lock);
        d.get()->offset = off + (uint32_t)res;
    }

    return res;
}

extern "C" int vfs_ioctl(int fd, uint32_t req, void* arg) {
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

extern "C" int vfs_close(int fd) {
    task_t* curr = proc_current();

    file_desc_t* d = nullptr;
    if (proc_fd_remove(curr, fd, &d) < 0 || !d) {
        return -1;
    }

    file_desc_release(d);
    return 0;
}

extern "C" vfs_node_t* vfs_create_node_from_path(const char* path) {
    if (!path || path[0] == '\0') {
        return 0;
    }

    task_t* curr = proc_current();
    VfsResolvedPath resolved;
    if (!curr || vfs_resolve_path(curr, path, &resolved) != 0 || !resolved.mount || !resolved.mount->instance || !resolved.mount->instance->type || !resolved.mount->instance->type->ops) {
        return 0;
    }

    if (!resolved.mount->instance->type->ops->create_node_from_path) {
        return 0;
    }

    vfs_node_t* node = resolved.mount->instance->type->ops->create_node_from_path(
        resolved.mount->instance,
        resolved.mount->mountpoint,
        resolved.rel,
        resolved.is_abs
    );

    if (node) {
        vfs_node_bind_instance(node, resolved.mount->instance);
    }

    return node;
}

extern "C" int vfs_mkdir(const char* path) {
    if (!path || path[0] == '\0') {
        return -1;
    }

    task_t* curr = proc_current();
    VfsResolvedPath resolved;
    if (!curr || vfs_resolve_path(curr, path, &resolved) != 0 || !resolved.mount || !resolved.mount->instance || !resolved.mount->instance->type || !resolved.mount->instance->type->ops) {
        return -1;
    }

    if (!resolved.mount->instance->type->ops->mkdir) {
        return -1;
    }

    return resolved.mount->instance->type->ops->mkdir(
        resolved.mount->instance,
        resolved.mount->mountpoint,
        resolved.rel,
        resolved.is_abs
    );
}

extern "C" int vfs_mkdirat(int dirfd, const char* path) {
    task_t* curr = proc_current();
    if (!curr || !path || path[0] == '\0') {
        return -1;
    }

    char base[VFS_PATH_MAX];
    if (vfs_dirfd_to_base(curr, dirfd, base, sizeof(base)) != 0) {
        return -1;
    }

    VfsResolvedPath resolved;
    if (vfs_resolve_path_base(curr, base, path, &resolved) != 0 || !resolved.mount || !resolved.mount->instance || !resolved.mount->instance->type || !resolved.mount->instance->type->ops) {
        return -1;
    }

    if (!resolved.mount->instance->type->ops->mkdir) {
        return -1;
    }

    return resolved.mount->instance->type->ops->mkdir(
        resolved.mount->instance,
        resolved.mount->mountpoint,
        resolved.rel,
        resolved.is_abs
    );
}

extern "C" int vfs_unlink(const char* path) {
    if (!path || path[0] == '\0') {
        return -1;
    }

    task_t* curr = proc_current();
    VfsResolvedPath resolved;
    if (!curr || vfs_resolve_path(curr, path, &resolved) != 0 || !resolved.mount || !resolved.mount->instance || !resolved.mount->instance->type || !resolved.mount->instance->type->ops) {
        return -1;
    }

    if (!resolved.mount->instance->type->ops->unlink) {
        return -1;
    }

    return resolved.mount->instance->type->ops->unlink(
        resolved.mount->instance,
        resolved.mount->mountpoint,
        resolved.rel,
        resolved.is_abs
    );
}

extern "C" int vfs_unlinkat(int dirfd, const char* path) {
    task_t* curr = proc_current();
    if (!curr || !path || path[0] == '\0') {
        return -1;
    }

    char base[VFS_PATH_MAX];
    if (vfs_dirfd_to_base(curr, dirfd, base, sizeof(base)) != 0) {
        return -1;
    }

    VfsResolvedPath resolved;
    if (vfs_resolve_path_base(curr, base, path, &resolved) != 0 || !resolved.mount || !resolved.mount->instance || !resolved.mount->instance->type || !resolved.mount->instance->type->ops) {
        return -1;
    }

    if (!resolved.mount->instance->type->ops->unlink) {
        return -1;
    }

    return resolved.mount->instance->type->ops->unlink(
        resolved.mount->instance,
        resolved.mount->mountpoint,
        resolved.rel,
        resolved.is_abs
    );
}

extern "C" int vfs_stat_path(const char* path, vfs_stat_t* out) {
    if (!path || path[0] == '\0' || !out) {
        return -1;
    }

    task_t* curr = proc_current();
    VfsResolvedPath resolved;
    if (!curr || vfs_resolve_path(curr, path, &resolved) != 0 || !resolved.mount || !resolved.mount->instance || !resolved.mount->instance->type || !resolved.mount->instance->type->ops) {
        return -1;
    }

    if (!resolved.mount->instance->type->ops->stat) {
        return -1;
    }

    return resolved.mount->instance->type->ops->stat(
        resolved.mount->instance,
        resolved.mount->mountpoint,
        resolved.rel,
        resolved.is_abs,
        out
    );
}

extern "C" int vfs_statat_path(int dirfd, const char* path, vfs_stat_t* out) {
    task_t* curr = proc_current();
    if (!curr || !path || path[0] == '\0' || !out) {
        return -1;
    }

    char base[VFS_PATH_MAX];
    if (vfs_dirfd_to_base(curr, dirfd, base, sizeof(base)) != 0) {
        return -1;
    }

    VfsResolvedPath resolved;
    if (vfs_resolve_path_base(curr, base, path, &resolved) != 0 || !resolved.mount || !resolved.mount->instance || !resolved.mount->instance->type || !resolved.mount->instance->type->ops) {
        return -1;
    }

    if (!resolved.mount->instance->type->ops->stat) {
        return -1;
    }

    return resolved.mount->instance->type->ops->stat(
        resolved.mount->instance,
        resolved.mount->mountpoint,
        resolved.rel,
        resolved.is_abs,
        out
    );
}

extern "C" int vfs_rename(const char* old_path, const char* new_path) {
    if (!old_path || old_path[0] == '\0' || !new_path || new_path[0] == '\0') {
        return -1;
    }

    task_t* curr = proc_current();
    VfsResolvedPath old_resolved;
    VfsResolvedPath new_resolved;
    if (!curr || vfs_resolve_path(curr, old_path, &old_resolved) != 0 || !old_resolved.mount || !old_resolved.mount->instance || !old_resolved.mount->instance->type || !old_resolved.mount->instance->type->ops) {
        return -1;
    }

    if (vfs_resolve_path(curr, new_path, &new_resolved) != 0 || !new_resolved.mount || !new_resolved.mount->instance || !new_resolved.mount->instance->type || !new_resolved.mount->instance->type->ops) {
        return -1;
    }

    if (old_resolved.mount->instance->type != new_resolved.mount->instance->type || old_resolved.mount->instance != new_resolved.mount->instance) {
        return -1;
    }

    if (!old_resolved.mount->instance->type->ops->rename) {
        return -1;
    }

    return old_resolved.mount->instance->type->ops->rename(
        old_resolved.mount->instance,
        old_resolved.mount->mountpoint,
        old_resolved.rel,
        new_resolved.mount->mountpoint,
        new_resolved.rel,
        old_resolved.is_abs,
        new_resolved.is_abs
    );
}

extern "C" int vfs_renameat(int old_dirfd, const char* old_path, int new_dirfd, const char* new_path) {
    task_t* curr = proc_current();
    if (!curr || !old_path || old_path[0] == '\0' || !new_path || new_path[0] == '\0') {
        return -1;
    }

    char old_base[VFS_PATH_MAX];
    char new_base[VFS_PATH_MAX];
    if (vfs_dirfd_to_base(curr, old_dirfd, old_base, sizeof(old_base)) != 0) {
        return -1;
    }
    if (vfs_dirfd_to_base(curr, new_dirfd, new_base, sizeof(new_base)) != 0) {
        return -1;
    }

    VfsResolvedPath old_resolved;
    VfsResolvedPath new_resolved;
    if (vfs_resolve_path_base(curr, old_base, old_path, &old_resolved) != 0 || !old_resolved.mount || !old_resolved.mount->instance || !old_resolved.mount->instance->type || !old_resolved.mount->instance->type->ops) {
        return -1;
    }

    if (vfs_resolve_path_base(curr, new_base, new_path, &new_resolved) != 0 || !new_resolved.mount || !new_resolved.mount->instance || !new_resolved.mount->instance->type || !new_resolved.mount->instance->type->ops) {
        return -1;
    }

    if (old_resolved.mount->instance->type != new_resolved.mount->instance->type || old_resolved.mount->instance != new_resolved.mount->instance) {
        return -1;
    }

    if (!old_resolved.mount->instance->type->ops->rename) {
        return -1;
    }

    return old_resolved.mount->instance->type->ops->rename(
        old_resolved.mount->instance,
        old_resolved.mount->mountpoint,
        old_resolved.rel,
        new_resolved.mount->mountpoint,
        new_resolved.rel,
        old_resolved.is_abs,
        new_resolved.is_abs
    );
}

extern "C" int vfs_get_fs_info(uint32_t* total_blocks, uint32_t* free_blocks, uint32_t* block_size) {
    if (!total_blocks || !free_blocks || !block_size) {
        return -1;
    }

    const vfs_mount_entry* root = nullptr;
    {
        kernel::SpinLockSafeGuard guard(g_mount_lock);
        root = vfs_mount_table_find_locked("/");
    }

    if (!root || !root->instance || !root->instance->type || !root->instance->type->ops || !root->instance->type->ops->get_fs_info) {
        return -1;
    }

    return root->instance->type->ops->get_fs_info(root->instance, total_blocks, free_blocks, block_size);
}
