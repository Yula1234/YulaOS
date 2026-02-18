#include <drivers/gpu0.h>
#include <drivers/virtio_gpu.h>

#include <fs/vfs.h>
#include <hal/lock.h>
#include <kernel/proc.h>
#include <kernel/shm.h>
#include <lib/hash_map.h>

#include <lib/cpp/intrusive_ref.h>
#include <lib/cpp/ioctl_dispatch.h>
#include <lib/cpp/new.h>
#include <lib/cpp/expected.h>
#include <lib/cpp/mutex.h>
#include <lib/cpp/unique_ptr.h>
#include <lib/cpp/utility.h>
#include <lib/cpp/vfs.h>
#include <lib/string.h>
#include <mm/heap.h>

#include <yos/gpu.h>

namespace kernel {
namespace gpu0 {

enum class Gpu0Error : uint32_t {
    InvalidArg,
    NotSupported,
    NotFound,
    OutOfMemory,
    VirtioError,
    ShmError,
};

using Gpu0Result = kernel::Expected<void, Gpu0Error>;

static int to_ioctl_rc(const Gpu0Result& r) {
    return r ? 0 : -1;
}

struct ResourceDesc {
    uint32_t width = 0u;
    uint32_t height = 0u;
    uint32_t depth = 0u;
    uint32_t format = 0u;

    uint8_t is_3d = 0u;
    uint8_t pad2[3]{};
};

struct BackingDesc {
    kernel::VirtualFSNode shm;

    uint32_t offset = 0u;
    uint32_t size_bytes = 0u;

    explicit operator bool() const {
        return (bool)shm && size_bytes != 0u;
    }

    void reset() {
        shm.reset();

        offset = 0u;
        size_bytes = 0u;
    }
};

class Slot {
public:
    uint32_t resource_id = 0u;

    ResourceDesc resource;
    BackingDesc backing;

    static uint32_t format_bpp(uint32_t format) {
        switch (format) {
            case YOS_GPU_FORMAT_B8G8R8X8_UNORM:
                return 4u;
            default:
                return 0u;
        }
    }

    bool min_size_bytes(uint32_t& out_size_bytes) const {
        if (resource.width == 0u
            || resource.height == 0u
            || resource.depth == 0u) {
            return false;
        }

        const uint32_t bpp = format_bpp(resource.format);

        if (bpp == 0u) {
            return false;
        }

        const uint64_t pixels =
            (uint64_t)resource.width
            * (uint64_t)resource.height
            * (uint64_t)resource.depth;

        if (pixels == 0ull
            || pixels > (0xFFFFFFFFull / (uint64_t)bpp)) {
            return false;
        }

        out_size_bytes = (uint32_t)(pixels * (uint64_t)bpp);

        return true;
    }

    bool validate_backing_offset(uint64_t offset) const {
        if (!backing) {
            return false;
        }

        const uint64_t size = (uint64_t)backing.size_bytes;

        if (size == 0u) {
            return false;
        }

        return offset < size;
    }

    bool validate_transfer_2d(
        uint32_t x,
        uint32_t y,
        uint32_t region_width,
        uint32_t region_height,
        uint64_t offset
    ) const {
        if (!backing) {
            return false;
        }

        if (resource.is_3d) {
            return false;
        }

        if (!validate_backing_offset(offset)) {
            return false;
        }

        if (region_width == 0u
            || region_height == 0u) {
            return false;
        }

        if (x >= resource.width
            || y >= resource.height) {
            return false;
        }

        if (region_width > resource.width - x) {
            return false;
        }

        if (region_height > resource.height - y) {
            return false;
        }

        const uint32_t bpp = format_bpp(resource.format);
        if (bpp == 0u) {
            return false;
        }

        const uint64_t stride = (uint64_t)resource.width * (uint64_t)bpp;

        if (stride == 0ull) {
            return false;
        }

        const uint64_t expected = (uint64_t)y * stride + (uint64_t)x * (uint64_t)bpp;

        if (offset != expected) {
            return false;
        }

        const uint64_t region_bytes =
            ((uint64_t)region_height - 1ull) * stride
            + (uint64_t)region_width * (uint64_t)bpp;

        if (region_bytes == 0ull) {
            return false;
        }

        const uint64_t end = offset + region_bytes;

        if (end < offset) {
            return false;
        }

        return end <= (uint64_t)backing.size_bytes;
    }

    bool validate_transfer_3d(
        uint32_t level,
        uint32_t stride,
        uint32_t layer_stride,
        const yos_gpu_box_t& box,
        uint64_t offset
    ) const {
        if (level != 0u) {
            return false;
        }

        if (!backing) {
            return false;
        }

        if (!resource.is_3d) {
            return false;
        }

        if (!validate_backing_offset(offset)) {
            return false;
        }

        if (box.w == 0u
            || box.h == 0u
            || box.d == 0u) {
            return false;
        }

        if (box.x >= resource.width
            || box.y >= resource.height
            || box.z >= resource.depth) {
            return false;
        }

        if (box.w > resource.width - box.x) {
            return false;
        }

        if (box.h > resource.height - box.y) {
            return false;
        }

        if (box.d > resource.depth - box.z) {
            return false;
        }

        const uint32_t bpp = format_bpp(resource.format);
        if (bpp == 0u) {
            return false;
        }

        const uint64_t min_stride = (uint64_t)resource.width * (uint64_t)bpp;

        if ((uint64_t)stride < min_stride) {
            return false;
        }

        if ((uint64_t)layer_stride < (uint64_t)resource.height * (uint64_t)stride) {
            return false;
        }

        const uint64_t expected =
            (uint64_t)box.z * (uint64_t)layer_stride
            + (uint64_t)box.y * (uint64_t)stride
            + (uint64_t)box.x * (uint64_t)bpp;

        if (offset != expected) {
            return false;
        }

        const uint64_t row_bytes = (uint64_t)box.w * (uint64_t)bpp;

        const uint64_t bytes =
            ((uint64_t)box.d - 1ull) * (uint64_t)layer_stride
            + ((uint64_t)box.h - 1ull) * (uint64_t)stride
            + row_bytes;

        if (bytes == 0ull) {
            return false;
        }

        const uint64_t end = offset + bytes;

        if (end < offset) {
            return false;
        }

        return end <= (uint64_t)backing.size_bytes;
    }
};

class SlotRecord {
public:
    SlotRecord() = default;

    SlotRecord(const SlotRecord&) = delete;
    SlotRecord& operator=(const SlotRecord&) = delete;

    SlotRecord(SlotRecord&&) = delete;
    SlotRecord& operator=(SlotRecord&&) = delete;

    bool retain() {
        kernel::SpinLockSafeGuard guard(ref_lock_);

        if (closing_ != 0u) {
            return false;
        }

        refcount_++;

        return true;
    }

    void release() {
        bool should_delete = false;

        {
            kernel::SpinLockSafeGuard guard(ref_lock_);
            if (refcount_ == 0u) {
                return;
            }

            refcount_--;
            should_delete = refcount_ == 0u;
        }

        if (should_delete) {
            delete this;
        }
    }

    void begin_close() {
        kernel::SpinLockSafeGuard guard(ref_lock_);

        closing_ = 1u;
    }

    bool is_closing() const {
        return closing_ != 0u;
    }

    kernel::Mutex& mutex() {
        return mutex_;
    }

    Slot& slot() {
        return slot_;
    }

    const Slot& slot() const {
        return slot_;
    }

private:
    mutable kernel::SpinLock ref_lock_;
    uint32_t refcount_ = 1u;
    uint32_t closing_ = 0u;

    kernel::Mutex mutex_;
    Slot slot_;
};

class GpuResourceHandle {
public:
    GpuResourceHandle() = default;

    GpuResourceHandle(const GpuResourceHandle&) = delete;
    GpuResourceHandle& operator=(const GpuResourceHandle&) = delete;

    GpuResourceHandle(GpuResourceHandle&& other) noexcept {
        move_from(other);
    }

    GpuResourceHandle& operator=(GpuResourceHandle&& other) noexcept {
        if (this == &other) {
            return *this;
        }

        reset();
        move_from(other);
        return *this;
    }

    ~GpuResourceHandle() {
        reset();
    }

    static bool create_2d(
        uint32_t resource_id,
        uint32_t format,
        uint32_t width,
        uint32_t height,
        GpuResourceHandle& out
    ) {
        if (resource_id == 0u) {
            return false;
        }

        if (virtio_gpu_resource_create_2d(resource_id, format, width, height) != 0) {
            return false;
        }

        out = GpuResourceHandle(resource_id);

        return true;
    }

    static bool create_3d(
        const yos_gpu_resource_create_3d_t& a,
        GpuResourceHandle& out
    ) {
        if (a.resource_id == 0u) {
            return false;
        }

        if (virtio_gpu_resource_create_3d(
                a.resource_id, a.target, a.format,
                a.bind, a.width, a.height, a.depth,
                a.array_size, a.last_level,
                a.nr_samples, a.flags
            ) != 0) {
            return false;
        }

        out = GpuResourceHandle(a.resource_id);

        return true;
    }

    uint32_t id() const {
        return resource_id_;
    }

    void disarm() {
        armed_ = false;
    }

private:
    explicit GpuResourceHandle(uint32_t resource_id)
        : resource_id_(resource_id),
          armed_(true) {
    }

    void move_from(GpuResourceHandle& other) {
        resource_id_ = other.resource_id_;
        armed_ = other.armed_;

        other.resource_id_ = 0u;
        other.armed_ = false;
    }

    void reset() {
        if (!armed_) {
            return;
        }

        if (resource_id_ != 0u) {
            (void)virtio_gpu_resource_unref(resource_id_);
        }

        resource_id_ = 0u;
        armed_ = false;
    }

private:
    uint32_t resource_id_ = 0u;
    bool armed_ = false;
};

class GpuBackingAttachGuard {
public:
    GpuBackingAttachGuard() = default;

    GpuBackingAttachGuard(uint32_t resource_id, kernel::VirtualFSNode shm, uint32_t offset, uint32_t size_bytes)
        : resource_id_(resource_id),
          shm_(kernel::move(shm)),
          offset_(offset),
          size_bytes_(size_bytes) {
    }

    GpuBackingAttachGuard(const GpuBackingAttachGuard&) = delete;
    GpuBackingAttachGuard& operator=(const GpuBackingAttachGuard&) = delete;

    GpuBackingAttachGuard(GpuBackingAttachGuard&& other) noexcept {
        move_from(other);
    }

    GpuBackingAttachGuard& operator=(GpuBackingAttachGuard&& other) noexcept {
        if (this == &other) {
            return *this;
        }

        reset();
        move_from(other);
        return *this;
    }

    ~GpuBackingAttachGuard() {
        reset();
    }

    bool attach_phys_pages(const uint32_t* pages, uint32_t page_count) {
        if (!shm_
            || resource_id_ == 0u) {
            return false;
        }

        if (attached_) {
            return false;
        }

        if (virtio_gpu_resource_attach_phys_pages(
                resource_id_, pages, page_count,
                offset_, size_bytes_
            ) != 0) {
            return false;
        }

        attached_ = true;
        return true;
    }

    bool commit_to_slot(Slot& slot) {
        if (!attached_
            || !shm_) {
            return false;
        }

        slot.backing.shm = kernel::move(shm_);
        slot.backing.offset = offset_;
        slot.backing.size_bytes = size_bytes_;

        attached_ = false;

        offset_ = 0u;
        size_bytes_ = 0u;

        return true;
    }

private:
    void move_from(GpuBackingAttachGuard& other) {
        resource_id_ = other.resource_id_;
        shm_ = kernel::move(other.shm_);
        
        offset_ = other.offset_;
        size_bytes_ = other.size_bytes_;
        attached_ = other.attached_;

        other.resource_id_ = 0u;

        other.offset_ = 0u;
        other.size_bytes_ = 0u;
        other.attached_ = false;
    }

    void reset() {
        if (attached_
            && resource_id_ != 0u) {
            (void)virtio_gpu_resource_detach_backing(resource_id_);
        }

        attached_ = false;

        resource_id_ = 0u;
        offset_ = 0u;
        size_bytes_ = 0u;

        shm_.reset();
    }

private:
    uint32_t resource_id_ = 0u;

    kernel::VirtualFSNode shm_;

    uint32_t offset_ = 0u;
    uint32_t size_bytes_ = 0u;

    bool attached_ = false;
};

class Context {
public:
    Context() = default;

    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;

    Context(Context&&) = delete;
    Context& operator=(Context&&) = delete;

    ~Context() {
        destroy_resources();
    }

    bool init() {
        return true;
    }

    bool find_and_retain(uint32_t resource_id, kernel::IntrusiveRef<SlotRecord>& out) {
        if (resource_id == 0u) {
            return false;
        }

        SlotRecord* ptr = nullptr;
        const bool ok = slots_.with_value_locked(
            resource_id,
            [&](SlotRecord*& v) -> bool {
                if (!v) {
                    return false;
                }

                if (!v->retain()) {
                    return false;
                }

                ptr = v;

                return true;
            }
        );

        if (!ok) {
            return false;
        }

        out = kernel::IntrusiveRef<SlotRecord>::adopt(ptr);

        return true;
    }

    template<typename F>
    bool with_slot_locked(uint32_t resource_id, F func) {
        if (resource_id == 0u) {
            return false;
        }

        kernel::IntrusiveRef<SlotRecord> rec;

        if (!find_and_retain(resource_id, rec)) {
            return false;
        }

        kernel::MutexGuard guard(rec->mutex());

        if (rec->is_closing()) {
            return false;
        }

        return func(rec->slot());
    }

    bool contains_locked(uint32_t resource_id) {
        if (resource_id == 0u) {
            return false;
        }

        kernel::IntrusiveRef<SlotRecord> rec;

        return find_and_retain(resource_id, rec);
    }

    bool insert_new_locked(uint32_t resource_id, Slot slot) {
        if (resource_id == 0u) {
            return false;
        }

        SlotRecord* rec = new SlotRecord();
        rec->slot() = kernel::move(slot);

        const auto result = slots_.insert_unique_ex(resource_id, rec);
        if (result == decltype(slots_)::InsertUniqueResult::Inserted) {
            return true;
        }

        rec->release();

        return false;
    }

    bool begin_close_and_remove(uint32_t resource_id, kernel::IntrusiveRef<SlotRecord>& out) {
        if (resource_id == 0u) {
            return false;
        }

        const bool marked = slots_.with_value_locked(
            resource_id,
            [&](SlotRecord*& v) -> bool {
                if (!v) {
                    return false;
                }

                v->begin_close();

                return true;
            }
        );

        if (!marked) {
            return false;
        }

        SlotRecord* rec = nullptr;
        if (!slots_.remove_and_get(resource_id, rec)) {
            return false;
        }

        if (!rec) {
            return false;
        }

        out = kernel::IntrusiveRef<SlotRecord>::adopt(rec);

        return true;
    }

    bool remove_locked(uint32_t resource_id) {
        if (resource_id == 0u) {
            return false;
        }

        SlotRecord* rec = nullptr;
        if (!slots_.remove_and_get(resource_id, rec)) {
            return false;
        }

        if (rec) {
            rec->begin_close();
            rec->release();
        }

        return true;
    }

private:
    void destroy_resources() {
        struct CleanupItem {
            uint32_t resource_id;
            kernel::VirtualFSNode shm;
            SlotRecord* rec;
        };

        CleanupItem* items = nullptr;
        uint32_t item_count = 0u;

        {
            auto view = slots_.locked_view();
            for (auto it = view.begin(); it != view.end(); ++it) {
                ++item_count;
            }

            if (item_count != 0u) {
                items = new (kernel::nothrow) CleanupItem[item_count];
                if (items) {
                    uint32_t i = 0u;
                    for (auto it = view.begin(); it != view.end(); ++it) {
                        auto pair = *it;
                        SlotRecord* rec = pair.second;
                        if (!rec) {
                            continue;
                        }

                        kernel::MutexGuard slot_guard(rec->mutex());
                        Slot& s = rec->slot();

                        items[i] = {
                            .resource_id = s.resource_id,
                            .shm = kernel::move(s.backing.shm),
                            .rec = rec,
                        };

                        s.backing.offset = 0u;
                        s.backing.size_bytes = 0u;

                        i++;
                    }

                    item_count = i;
                } else {
                    item_count = 0u;
                }
            }
        }

        slots_.clear();

        if (!items
            || item_count == 0u) {
            if (items) {
                delete[] items;
            }

            return;
        }

        for (uint32_t i = 0; i < item_count; ++i) {
            const uint32_t resource_id = items[i].resource_id;
            kernel::VirtualFSNode shm_node = kernel::move(items[i].shm);
            SlotRecord* rec = items[i].rec;

            if (shm_node) {
                (void)virtio_gpu_resource_detach_backing(resource_id);
                shm_node.reset();
            }

            (void)virtio_gpu_resource_unref(resource_id);

            if (rec) {
                rec->begin_close();
                rec->release();
            }
        }

        delete[] items;
    }

private:
    HashMap<uint32_t, SlotRecord*, 128> slots_;
};

static int ioctl_get_info(Context& ctx, yos_gpu_info_t& info);

static int ioctl_resource_attach_shm(Context& ctx, const yos_gpu_resource_attach_shm_t& a);
static int ioctl_resource_copy_region_3d(Context& ctx, const yos_gpu_copy_region_3d_t& a);
static int ioctl_resource_create_2d(Context& ctx, const yos_gpu_resource_create_2d_t& a);
static int ioctl_resource_create_3d(Context& ctx, const yos_gpu_resource_create_3d_t& a);

static int ioctl_resource_detach_backing(Context& ctx, uint32_t resource_id);
static int ioctl_resource_flush(Context& ctx, const yos_gpu_rect_t& a);
static int ioctl_resource_unref(Context& ctx, uint32_t resource_id);

static int ioctl_set_scanout(Context& ctx, const yos_gpu_set_scanout_t& a);

static int ioctl_transfer_to_host_2d(Context& ctx, const yos_gpu_transfer_to_host_2d_t& a);
static int ioctl_transfer_to_host_3d(Context& ctx, const yos_gpu_transfer_host_3d_t& a);

static const kernel::IoctlDispatcher::Entry kIoctlTable[] = {
    {
        YOS_GPU_GET_INFO,
        kernel::IoctlDispatcher::adapt_inout<Context, yos_gpu_info_t, ioctl_get_info>,
    },
    {
        YOS_GPU_RESOURCE_ATTACH_SHM,
        kernel::IoctlDispatcher::adapt_in<
            Context,
            yos_gpu_resource_attach_shm_t,
            ioctl_resource_attach_shm
        >,
    },
    {
        YOS_GPU_RESOURCE_COPY_REGION_3D,
        kernel::IoctlDispatcher::adapt_in<
            Context,
            yos_gpu_copy_region_3d_t,
            ioctl_resource_copy_region_3d
        >,
    },
    {
        YOS_GPU_RESOURCE_CREATE_2D,
        kernel::IoctlDispatcher::adapt_in<
            Context,
            yos_gpu_resource_create_2d_t,
            ioctl_resource_create_2d
        >,
    },
    {
        YOS_GPU_RESOURCE_CREATE_3D,
        kernel::IoctlDispatcher::adapt_in<
            Context,
            yos_gpu_resource_create_3d_t,
            ioctl_resource_create_3d
        >,
    },
    {
        YOS_GPU_RESOURCE_DETACH_BACKING,
        kernel::IoctlDispatcher::adapt_value_in<
            Context,
            uint32_t,
            ioctl_resource_detach_backing
        >,
    },
    {
        YOS_GPU_RESOURCE_FLUSH,
        kernel::IoctlDispatcher::adapt_in<
            Context,
            yos_gpu_rect_t,
            ioctl_resource_flush
        >,
    },
    {
        YOS_GPU_RESOURCE_UNREF,
        kernel::IoctlDispatcher::adapt_value_in<
            Context,
            uint32_t,
            ioctl_resource_unref
        >,
    },
    {
        YOS_GPU_SET_SCANOUT,
        kernel::IoctlDispatcher::adapt_in<
            Context,
            yos_gpu_set_scanout_t,
            ioctl_set_scanout
        >,
    },
    {
        YOS_GPU_TRANSFER_TO_HOST_2D,
        kernel::IoctlDispatcher::adapt_in<
            Context,
            yos_gpu_transfer_to_host_2d_t,
            ioctl_transfer_to_host_2d
        >,
    },
    {
        YOS_GPU_TRANSFER_TO_HOST_3D,
        kernel::IoctlDispatcher::adapt_in<
            Context,
            yos_gpu_transfer_host_3d_t,
            ioctl_transfer_to_host_3d
        >,
    },
};

class Gpu0Driver {
public:
    Gpu0Driver() = default;

    Gpu0Driver(const Gpu0Driver&) = delete;
    Gpu0Driver& operator=(const Gpu0Driver&) = delete;

    Gpu0Driver(Gpu0Driver&&) = delete;
    Gpu0Driver& operator=(Gpu0Driver&&) = delete;

    bool init() {
        return context_.init();
    }

    int ioctl(uint32_t req, void* arg) {
        return dispatcher_.dispatch(&context_, req, arg);
    }

private:
    Context context_;
    kernel::IoctlDispatcher dispatcher_{kIoctlTable};
};

static Gpu0Driver* driver_from_node(vfs_node_t* node) {
    if (!node) {
        return nullptr;
    }

    if (node->private_data == nullptr) {
        return nullptr;
    }

    return (Gpu0Driver*)node->private_data;
}

static vfs_node_t* fd_to_node(int32_t fd) {
    task_t* curr = proc_current();

    if (!curr) {
        return nullptr;
    }

    file_desc_t* d = proc_fd_get(curr, (int)fd);

    if (!d) {
        return nullptr;
    }

    if (!d->node) {
        file_desc_release(d);

        return nullptr;
    }

    vfs_node_t* node = d->node;
    file_desc_release(d);

    return node;
}

static int vfs_open(vfs_node_t* node) {
    if (!node) {
        return -1;
    }

    Gpu0Driver* driver = new (kernel::nothrow) Gpu0Driver();
    if (!driver) {
        return -1;
    }

    if (!driver->init()) {
        delete driver;

        return -1;
    }

    node->private_data = driver;

    return 0;
}

static int vfs_close(vfs_node_t* node) {
    if (!node) {
        return -1;
    }

    Gpu0Driver* driver = driver_from_node(node);

    if (driver) {
        delete driver;

        node->private_data = nullptr;
    }

    if ((node->flags & VFS_FLAG_DEVFS_ALLOC) != 0u) {
        kfree(node);
    }

    return 0;
}

static Gpu0Result ioctl_get_info_impl(Context&, yos_gpu_info_t& info) {
    const int active = virtio_gpu_is_active();
    const int virgl = virtio_gpu_virgl_is_supported();

    const virtio_gpu_fb_t* fb = virtio_gpu_get_fb();

    memset(&info, 0, sizeof(info));

    info.abi_version = YOS_GPU_ABI_VERSION;

    info.flags = active ? YOS_GPU_INFO_FLAG_ACTIVE : 0u;

    if (virgl) {
        info.flags |= YOS_GPU_INFO_FLAG_VIRGL;
    }

    info.width = fb ? fb->width : 0u;
    info.height = fb ? fb->height : 0u;

    info.scanout_id = virtio_gpu_get_scanout_id();

    return Gpu0Result::success();
}

static int ioctl_get_info(Context& ctx, yos_gpu_info_t& info) {
    return to_ioctl_rc(ioctl_get_info_impl(ctx, info));
}

static Gpu0Result ioctl_resource_create_2d_impl(Context& ctx, const yos_gpu_resource_create_2d_t& a) {
    if (a.resource_id == 0u) {
        return Gpu0Result::failure(Gpu0Error::InvalidArg);
    }

    if (a.width == 0u
        || a.height == 0u) {
        return Gpu0Result::failure(Gpu0Error::InvalidArg);
    }

    if (Slot::format_bpp(a.format) == 0u) {
        return Gpu0Result::failure(Gpu0Error::InvalidArg);
    }

    Slot slot;

    slot.resource_id = a.resource_id;
    slot.backing.reset();

    slot.resource.width = a.width;
    slot.resource.height = a.height;
    slot.resource.depth = 1u;
    slot.resource.format = a.format;
    slot.resource.is_3d = 0u;

    GpuResourceHandle resource;

    if (!GpuResourceHandle::create_2d(a.resource_id, a.format, a.width, a.height, resource)) {
        return Gpu0Result::failure(Gpu0Error::VirtioError);
    }

    if (!ctx.insert_new_locked(a.resource_id, kernel::move(slot))) {
        return Gpu0Result::failure(Gpu0Error::OutOfMemory);
    }

    resource.disarm();

    return Gpu0Result::success();
}

static int ioctl_resource_create_2d(Context& ctx, const yos_gpu_resource_create_2d_t& a) {
    return to_ioctl_rc(ioctl_resource_create_2d_impl(ctx, a));
}

static Gpu0Result ioctl_resource_create_3d_impl(Context& ctx, const yos_gpu_resource_create_3d_t& a) {
    if (!virtio_gpu_virgl_is_supported()) {
        return Gpu0Result::failure(Gpu0Error::NotSupported);
    }

    if (a.resource_id == 0u) {
        return Gpu0Result::failure(Gpu0Error::InvalidArg);
    }

    if (a.width == 0u
        || a.height == 0u
        || a.depth == 0u) {
        return Gpu0Result::failure(Gpu0Error::InvalidArg);
    }

    if (Slot::format_bpp(a.format) == 0u) {
        return Gpu0Result::failure(Gpu0Error::InvalidArg);
    }

    Slot slot;
    slot.resource_id = a.resource_id;
    slot.backing.reset();

    slot.resource.width = a.width;
    slot.resource.height = a.height;
    slot.resource.depth = a.depth;
    slot.resource.format = a.format;
    slot.resource.is_3d = 1u;

    GpuResourceHandle resource;

    if (!GpuResourceHandle::create_3d(a, resource)) {
        return Gpu0Result::failure(Gpu0Error::VirtioError);
    }

    if (!ctx.insert_new_locked(a.resource_id, kernel::move(slot))) {
        return Gpu0Result::failure(Gpu0Error::OutOfMemory);
    }

    resource.disarm();

    return Gpu0Result::success();
}

static int ioctl_resource_create_3d(Context& ctx, const yos_gpu_resource_create_3d_t& a) {
    return to_ioctl_rc(ioctl_resource_create_3d_impl(ctx, a));
}

static Gpu0Result ioctl_resource_attach_shm_impl(Context& ctx, const yos_gpu_resource_attach_shm_t& a) {
    if (a.resource_id == 0u) {
        return Gpu0Result::failure(Gpu0Error::InvalidArg);
    }

    if (a.shm_fd < 0) {
        return Gpu0Result::failure(Gpu0Error::InvalidArg);
    }

    if (a.size_bytes == 0u) {
        return Gpu0Result::failure(Gpu0Error::InvalidArg);
    }

    uint32_t min_size = 0u;

    {
        const bool ok = ctx.with_slot_locked(
            a.resource_id,
            [&](Slot& s) -> bool {
                if (s.min_size_bytes(min_size)
                    && a.size_bytes < min_size) {
                    return false;
                }

                return true;
            }
        );

        if (!ok) {
            return Gpu0Result::failure(Gpu0Error::InvalidArg);
        }
    }

    vfs_node_t* shm_node_raw = fd_to_node(a.shm_fd);

    if (!shm_node_raw) {
        return Gpu0Result::failure(Gpu0Error::InvalidArg);
    }

    kernel::VirtualFSNode shm_node = kernel::VirtualFSNode::retained(shm_node_raw);

    auto view = kernel::shm::ShmNodeView::from_node(shm_node.get());

    if (!view) {
        return Gpu0Result::failure(Gpu0Error::InvalidArg);
    }

    if (!view.value().validate_range(a.shm_offset, a.size_bytes)) {
        return Gpu0Result::failure(Gpu0Error::InvalidArg);
    }

    const uint32_t* pages = nullptr;
    uint32_t page_count = 0u;

    if (!view.value().phys_pages(pages, page_count)) {
        return Gpu0Result::failure(Gpu0Error::ShmError);
    }

    kernel::VirtualFSNode old_shm;
    uint32_t old_offset = 0u;
    uint32_t old_size_bytes = 0u;

    {
        const bool ok = ctx.with_slot_locked(
            a.resource_id,
            [&](Slot& s) -> bool {
                old_shm = kernel::move(s.backing.shm);

                old_offset = s.backing.offset;
                old_size_bytes = s.backing.size_bytes;

                s.backing.reset();

                return true;
            }
        );

        if (!ok) {
            return Gpu0Result::failure(Gpu0Error::NotFound);
        }
    }

    if (old_shm) {
        if (virtio_gpu_resource_detach_backing(a.resource_id) != 0) {
            const bool restored = ctx.with_slot_locked(
                a.resource_id,
                [&](Slot& s) -> bool {
                    if (s.backing) {
                        return false;
                    }

                    s.backing.shm = kernel::move(old_shm);

                    s.backing.offset = old_offset;
                    s.backing.size_bytes = old_size_bytes;

                    return true;
                }
            );

            if (!restored) {
                old_shm.reset();
            }

            return Gpu0Result::failure(Gpu0Error::VirtioError);
        }

        old_shm.reset();
    }

    GpuBackingAttachGuard attach_guard(
        a.resource_id, kernel::move(shm_node),
        a.shm_offset, a.size_bytes
    );

    if (!attach_guard.attach_phys_pages(pages, page_count)) {
        return Gpu0Result::failure(Gpu0Error::VirtioError);
    }

    {
        const bool ok = ctx.with_slot_locked(
            a.resource_id,
            [&](Slot& s) -> bool {
                return attach_guard.commit_to_slot(s);
            }
        );

        if (!ok) {
            return Gpu0Result::failure(Gpu0Error::NotFound);
        }
    }

    return Gpu0Result::success();
}

static int ioctl_resource_attach_shm(Context& ctx, const yos_gpu_resource_attach_shm_t& a) {
    return to_ioctl_rc(ioctl_resource_attach_shm_impl(ctx, a));
}

static Gpu0Result ioctl_resource_detach_backing_impl(Context& ctx, uint32_t resource_id) {
    if (resource_id == 0u) {
        return Gpu0Result::failure(Gpu0Error::InvalidArg);
    }

    kernel::VirtualFSNode shm_node;

    {
        const bool ok = ctx.with_slot_locked(
            resource_id,
            [&shm_node](Slot& s) -> bool {
                shm_node = kernel::move(s.backing.shm);
                s.backing.offset = 0u;
                s.backing.size_bytes = 0u;

                return true;
            }
        );

        if (!ok) {
            return Gpu0Result::failure(Gpu0Error::NotFound);
        }
    }

    const int rc = virtio_gpu_resource_detach_backing(resource_id);

    shm_node.reset();

    if (rc != 0) {
        return Gpu0Result::failure(Gpu0Error::VirtioError);
    }

    return Gpu0Result::success();
}

static int ioctl_resource_detach_backing(Context& ctx, uint32_t resource_id) {
    return to_ioctl_rc(ioctl_resource_detach_backing_impl(ctx, resource_id));
}

static Gpu0Result ioctl_resource_unref_impl(Context& ctx, uint32_t resource_id) {
    if (resource_id == 0u) {
        return Gpu0Result::failure(Gpu0Error::InvalidArg);
    }

    kernel::IntrusiveRef<SlotRecord> rec;
    if (!ctx.begin_close_and_remove(resource_id, rec)) {
        return Gpu0Result::failure(Gpu0Error::NotFound);
    }

    kernel::VirtualFSNode shm_node;

    {
        kernel::MutexGuard guard(rec->mutex());
        Slot& s = rec->slot();

        shm_node = kernel::move(s.backing.shm);
        s.backing.offset = 0u;
        s.backing.size_bytes = 0u;
    }

    if (shm_node) {
        (void)virtio_gpu_resource_detach_backing(resource_id);
        shm_node.reset();
    }

    const int rc = virtio_gpu_resource_unref(resource_id);

    if (rc != 0) {
        return Gpu0Result::failure(Gpu0Error::VirtioError);
    }

    return Gpu0Result::success();
}

static int ioctl_resource_unref(Context& ctx, uint32_t resource_id) {
    return to_ioctl_rc(ioctl_resource_unref_impl(ctx, resource_id));
}

static Gpu0Result ioctl_set_scanout_impl(Context& ctx, const yos_gpu_set_scanout_t& a) {
    if (a.resource_id == 0u) {
        return Gpu0Result::failure(Gpu0Error::InvalidArg);
    }

    if (a.scanout_id >= YOS_GPU_MAX_SCANOUTS) {
        return Gpu0Result::failure(Gpu0Error::InvalidArg);
    }

    if (!ctx.contains_locked(a.resource_id)) {
        return Gpu0Result::failure(Gpu0Error::NotFound);
    }

    const int rc = virtio_gpu_set_scanout(
        a.scanout_id, a.resource_id,
        a.x, a.y,
        a.width, a.height
    );

    if (rc != 0) {
        return Gpu0Result::failure(Gpu0Error::VirtioError);
    }

    return Gpu0Result::success();
}

static int ioctl_set_scanout(Context& ctx, const yos_gpu_set_scanout_t& a) {
    return to_ioctl_rc(ioctl_set_scanout_impl(ctx, a));
}

static Gpu0Result ioctl_transfer_to_host_2d_impl(Context& ctx, const yos_gpu_transfer_to_host_2d_t& a) {
    if (a.resource_id == 0u) {
        return Gpu0Result::failure(Gpu0Error::InvalidArg);
    }

    if (a.width == 0u
        || a.height == 0u) {
        return Gpu0Result::failure(Gpu0Error::InvalidArg);
    }

    const bool ok = ctx.with_slot_locked(
        a.resource_id,
        [&](Slot& s) -> bool {
            return s.validate_transfer_2d(a.x, a.y, a.width, a.height, a.offset);
        }
    );

    if (!ok) {
        return Gpu0Result::failure(Gpu0Error::InvalidArg);
    }

    const int rc = virtio_gpu_transfer_to_host_2d(
        a.resource_id, a.x, a.y, a.width,
        a.height, a.offset
    );

    if (rc != 0) {
        return Gpu0Result::failure(Gpu0Error::VirtioError);
    }

    return Gpu0Result::success();
}

static int ioctl_transfer_to_host_2d(Context& ctx, const yos_gpu_transfer_to_host_2d_t& a) {
    return to_ioctl_rc(ioctl_transfer_to_host_2d_impl(ctx, a));
}

static Gpu0Result ioctl_transfer_to_host_3d_impl(Context& ctx, const yos_gpu_transfer_host_3d_t& a) {
    if (!virtio_gpu_virgl_is_supported()) {
        return Gpu0Result::failure(Gpu0Error::NotSupported);
    }

    if (a.resource_id == 0u) {
        return Gpu0Result::failure(Gpu0Error::InvalidArg);
    }

    if (a.level != 0u) {
        return Gpu0Result::failure(Gpu0Error::InvalidArg);
    }

    if (a.box.w == 0u
        || a.box.h == 0u
        || a.box.d == 0u) {
        return Gpu0Result::failure(Gpu0Error::InvalidArg);
    }

    const bool ok = ctx.with_slot_locked(
        a.resource_id,
        [&](Slot& s) -> bool {
            return s.validate_transfer_3d(
                a.level, a.stride,
                a.layer_stride,
                a.box, a.offset
            );
        }
    );

    if (!ok) {
        return Gpu0Result::failure(Gpu0Error::InvalidArg);
    }

    const int rc = virtio_gpu_transfer_to_host_3d(
        a.resource_id, a.level,
        a.stride, a.layer_stride,
        a.box.x, a.box.y, a.box.z,
        a.box.w, a.box.h, a.box.d,
        a.offset
    );

    if (rc != 0) {
        return Gpu0Result::failure(Gpu0Error::VirtioError);
    }

    return Gpu0Result::success();
}

static int ioctl_transfer_to_host_3d(Context& ctx, const yos_gpu_transfer_host_3d_t& a) {
    return to_ioctl_rc(ioctl_transfer_to_host_3d_impl(ctx, a));
}

static Gpu0Result ioctl_resource_copy_region_3d_impl(Context& ctx, const yos_gpu_copy_region_3d_t& a) {
    if (!virtio_gpu_virgl_is_supported()) {
        return Gpu0Result::failure(Gpu0Error::NotSupported);
    }

    if (a.dst_resource_id == 0u
        || a.src_resource_id == 0u) {
        return Gpu0Result::failure(Gpu0Error::InvalidArg);
    }

    if (a.dst_level != 0u
        || a.src_level != 0u) {
        return Gpu0Result::failure(Gpu0Error::InvalidArg);
    }

    if (a.width == 0u
        || a.height == 0u
        || a.depth == 0u) {
        return Gpu0Result::failure(Gpu0Error::InvalidArg);
    }

    const bool ok = ctx.with_slot_locked(
        a.dst_resource_id,
        [&](Slot& dst) -> bool {
            return ctx.with_slot_locked(
                a.src_resource_id,
                [&](Slot& src) -> bool {
                    if (!dst.resource.is_3d
                        || !src.resource.is_3d) {
                        return false;
                    }

                    if (a.dst_x >= dst.resource.width
                        || a.dst_y >= dst.resource.height
                        || a.dst_z >= dst.resource.depth) {
                        return false;
                    }

                    if (a.src_x >= src.resource.width
                        || a.src_y >= src.resource.height
                        || a.src_z >= src.resource.depth) {
                        return false;
                    }

                    if (a.width > dst.resource.width - a.dst_x
                        || a.height > dst.resource.height - a.dst_y
                        || a.depth > dst.resource.depth - a.dst_z) {
                        return false;
                    }

                    if (a.width > src.resource.width - a.src_x
                        || a.height > src.resource.height - a.src_y
                        || a.depth > src.resource.depth - a.src_z) {
                        return false;
                    }

                    return true;
                }
            );
        }
    );

    if (!ok) {
        return Gpu0Result::failure(Gpu0Error::InvalidArg);
    }

    const int rc = virtio_gpu_virgl_copy_region(
        a.dst_resource_id, a.dst_level,
        a.dst_x, a.dst_y, a.dst_z,
        a.src_resource_id, a.src_level,
        a.src_x, a.src_y, a.src_z,
        a.width, a.height, a.depth
    );

    if (rc != 0) {
        return Gpu0Result::failure(Gpu0Error::VirtioError);
    }

    return Gpu0Result::success();
}

static int ioctl_resource_copy_region_3d(Context& ctx, const yos_gpu_copy_region_3d_t& a) {
    return to_ioctl_rc(ioctl_resource_copy_region_3d_impl(ctx, a));
}

static Gpu0Result ioctl_resource_flush_impl(Context& ctx, const yos_gpu_rect_t& a) {
    if (a.resource_id == 0u) {
        return Gpu0Result::failure(Gpu0Error::InvalidArg);
    }

    if (a.width == 0u
        || a.height == 0u) {
        return Gpu0Result::failure(Gpu0Error::InvalidArg);
    }

    if (!ctx.contains_locked(a.resource_id)) {
        return Gpu0Result::failure(Gpu0Error::NotFound);
    }

    const int rc = virtio_gpu_resource_flush(
        a.resource_id,
        a.x, a.y,
        a.width, a.height
    );

    if (rc != 0) {
        return Gpu0Result::failure(Gpu0Error::VirtioError);
    }

    return Gpu0Result::success();
}

static int ioctl_resource_flush(Context& ctx, const yos_gpu_rect_t& a) {
    return to_ioctl_rc(ioctl_resource_flush_impl(ctx, a));
}

static int vfs_ioctl(vfs_node_t* node, uint32_t req, void* arg) {
    if (!node) {
        return -1;
    }

    Gpu0Driver* driver = driver_from_node(node);

    if (!driver) {
        return -1;
    }

    return driver->ioctl(req, arg);
}

static vfs_ops_t g_gpu0_ops = {
    .read = nullptr,
    .write = nullptr,

    .open = vfs_open,
    .close = vfs_close,
    .ioctl = vfs_ioctl,
};

static vfs_node_t g_gpu0_node = {
    .name = "gpu0",
    .flags = 0u,
    .size = 0u,
    .inode_idx = 0u,
    .refs = 0u,
    .ops = &g_gpu0_ops,
    
    .private_data = nullptr,
    .private_retain = nullptr,
    .private_release = nullptr,
};

}
}

extern "C" {

void gpu0_vfs_init(void) {
    devfs_register(&kernel::gpu0::g_gpu0_node);
}

}
