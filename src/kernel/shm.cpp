// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 Yula1234

#include <kernel/shm.h>

#include <fs/vfs.h>

#include <lib/hash_map.h>

#include <lib/cpp/atomic.h>
#include <lib/cpp/intrusive_ref.h>
#include <lib/cpp/new.h>
#include <lib/cpp/string.h>
#include <lib/cpp/unique_ptr.h>

#include <lib/string.h>

#ifdef __cplusplus
extern "C" {
#endif

#include <mm/pmm.h>
#include <arch/i386/paging.h>

#ifdef __cplusplus
}
#endif

namespace kernel {
namespace shm {

static constexpr uint32_t k_page_size = 4096u;
static constexpr uint32_t k_name_max_len = 31u;

class ShmObject {
public:
    static kernel::IntrusiveRef<ShmObject> create(uint32_t size) {
        if (size == 0u) {
            return {};
        }

        const uint32_t page_count = (size + (k_page_size - 1u)) / k_page_size;

        if (page_count == 0u) {
            return {};
        }

        const size_t pages_align = alignof(uint32_t);
        const size_t pages_off = (sizeof(ShmObject) + pages_align - 1u)
            & ~(pages_align - 1u);
        const size_t alloc_size = pages_off + sizeof(uint32_t) * (size_t)page_count;

        void* raw = ::operator new(alloc_size, kernel::nothrow);

        if (!raw) {
            return {};
        }

        struct ObjectDeleter {
            void operator()(ShmObject* p) const noexcept {
                if (!p) {
                    return;
                }

                p->~ShmObject();
                ::operator delete(p);
            }
        };

        uint32_t* pages = (uint32_t*)((unsigned char*)raw + pages_off);
        memset(pages, 0, sizeof(uint32_t) * (size_t)page_count);

        kernel::unique_ptr<ShmObject, ObjectDeleter> obj(
            new (raw) ShmObject(
                size,
                page_count,
                pages
            )
        );

        if (!obj) {
            ::operator delete(raw);
            return {};
        }

        if (!obj->allocate_phys_pages()) {
            return {};
        }

        return kernel::IntrusiveRef<ShmObject>::adopt(obj.release());
    }

    ShmObject(const ShmObject&) = delete;
    ShmObject& operator=(const ShmObject&) = delete;

    ShmObject(ShmObject&&) = delete;
    ShmObject& operator=(ShmObject&&) = delete;

    void retain() {
        refcount_.fetch_add(1u, kernel::memory_order::seq_cst);
    }

    void release() {
        if (refcount_.fetch_sub(1u, kernel::memory_order::seq_cst) != 1u) {
            return;
        }

        delete this;
    }

    uint32_t size() const {
        return size_;
    }

    bool get_phys_pages(const uint32_t*& out_pages, uint32_t& out_page_count) const {
        out_pages = pages_;
        out_page_count = page_count_;

        return out_pages && out_page_count != 0u;
    }

private:
    ShmObject(uint32_t size, uint32_t page_count, uint32_t* pages)
        : size_(size),
          page_count_(page_count),
          pages_(pages) {
    }

    ~ShmObject() {
        if (!pages_) {
            return;
        }

        for (uint32_t i = 0; i < page_count_; i++) {
            const uint32_t phys = pages_[i];

            if (phys != 0u) {
                pmm_free_block((void*)phys);
            }
        }
    }

    bool allocate_phys_pages() {
        class PhysPagesBuilder {
        public:
            PhysPagesBuilder(uint32_t* pages, uint32_t page_count)
                : pages_(pages),
                  capacity_(page_count) {
            }

            PhysPagesBuilder(const PhysPagesBuilder&) = delete;
            PhysPagesBuilder& operator=(const PhysPagesBuilder&) = delete;

            PhysPagesBuilder(PhysPagesBuilder&&) = delete;
            PhysPagesBuilder& operator=(PhysPagesBuilder&&) = delete;

            ~PhysPagesBuilder() {
                if (committed_) {
                    return;
                }

                if (!pages_) {
                    return;
                }

                for (uint32_t i = 0; i < allocated_; i++) {
                    const uint32_t phys = pages_[i];
                    if (phys != 0u) {
                        pmm_free_block((void*)phys);
                    }

                    pages_[i] = 0u;
                }
            }

            bool allocate_and_append_zeroed_page() {
                if (!pages_ || allocated_ >= capacity_) {
                    return false;
                }

                void* p = pmm_alloc_block();

                if (!p) {
                    return false;
                }

                paging_zero_phys_page((uint32_t)p);

                pages_[allocated_] = (uint32_t)p;
                allocated_++;

                return true;
            }

            void commit() {
                committed_ = true;
            }

        private:
            uint32_t* pages_ = nullptr;
            uint32_t capacity_ = 0u;
            uint32_t allocated_ = 0u;
            bool committed_ = false;
        };

        PhysPagesBuilder builder(pages_, page_count_);

        for (uint32_t i = 0; i < page_count_; i++) {
            if (!builder.allocate_and_append_zeroed_page()) {
                return false;
            }
        }

        builder.commit();

        return true;
    }

private:
    const uint32_t size_;

    const uint32_t page_count_;
    uint32_t* const pages_;

    kernel::atomic<uint32_t> refcount_{1u};
};

class ShmRegistry {
public:
    bool insert_unique(const kernel::string& name, ShmObject* obj) {
        obj->retain();

        const auto r = named_.insert_unique_ex(name, obj);

        if (r != decltype(named_)::InsertUniqueResult::Inserted) {
            obj->release();

            return false;
        }

        return true;
    }

    kernel::IntrusiveRef<ShmObject> find_and_retain(const kernel::string& name) {
        ShmObject* obj = nullptr;

        if (!named_.with_value_locked(
            name,
            [&obj](ShmObject* o) -> bool {
                if (!o) {
                    return false;
                }

                o->retain();
                obj = o;

                return true;
            }
        )) {
            return {};
        }

        return kernel::IntrusiveRef<ShmObject>::adopt(obj);
    }

    kernel::IntrusiveRef<ShmObject> remove(const kernel::string& name) {
        ShmObject* removed = nullptr;

        (void)named_.with_value_locked(
            name,
            [&removed](ShmObject* o) -> bool {
                removed = o;

                return true;
            }
        );

        if (!removed) {
            return {};
        }

        if (!named_.remove(name)) {
            return {};
        }

        return kernel::IntrusiveRef<ShmObject>::adopt(removed);
    }

private:
    HashMap<kernel::string, ShmObject*, 128> named_;
};

static kernel::atomic<uint32_t> g_registry_state{0u};
alignas(ShmRegistry) static unsigned char g_registry_storage[sizeof(ShmRegistry)];

static ShmRegistry& registry() {
    const uint32_t st = g_registry_state.load(kernel::memory_order::acquire);

    if (st == 2u) {
        return *(ShmRegistry*)g_registry_storage;
    }

    uint32_t expected = 0u;

    if (g_registry_state.compare_exchange_strong(
        expected,
        1u,
        kernel::memory_order::acq_rel,
        kernel::memory_order::acquire
    )) {
        new (g_registry_storage) ShmRegistry();
        g_registry_state.store(2u, kernel::memory_order::release);

        return *(ShmRegistry*)g_registry_storage;
    }

    kernel::spin_wait_equals(g_registry_state, 2u, kernel::memory_order::acquire);

    return *(ShmRegistry*)g_registry_storage;
}

static uint32_t name_len_bounded(const char* name) {
    if (!name) {
        return 0u;
    }

    for (uint32_t i = 0; i < k_name_max_len; i++) {
        if (name[i] == '\0') {
            return i;
        }
    }

    return 0u;
}

struct ShmNodeData {
    kernel::IntrusiveRef<ShmObject> obj;

    explicit ShmNodeData(kernel::IntrusiveRef<ShmObject>&& in)
        : obj(kernel::move(in)) {
    }

    ShmNodeData(const ShmNodeData&) = delete;
    ShmNodeData& operator=(const ShmNodeData&) = delete;

    ShmNodeData(ShmNodeData&&) = delete;
    ShmNodeData& operator=(ShmNodeData&&) = delete;
};

void shm_object_retain(ShmObject* obj) {
    if (!obj) {
        return;
    }

    obj->retain();
}

void shm_object_release(ShmObject* obj) {
    if (!obj) {
        return;
    }

    obj->release();
}

kernel::Expected<ShmObject*, ShmViewError>
shm_retain_object_from_node(vfs_node_t* node) {
    if (!node) {
        return kernel::Expected<ShmObject*, ShmViewError>::failure(
            ShmViewError::InvalidArg
        );
    }

    if ((node->flags & VFS_FLAG_SHM) == 0u) {
        return kernel::Expected<ShmObject*, ShmViewError>::failure(
            ShmViewError::NotShmNode
        );
    }

    auto* data = (ShmNodeData*)node->private_data;

    if (!data || !data->obj) {
        return kernel::Expected<ShmObject*, ShmViewError>::failure(
            ShmViewError::CorruptNode
        );
    }

    ShmObject* obj = data->obj.get();

    if (!obj) {
        return kernel::Expected<ShmObject*, ShmViewError>::failure(
            ShmViewError::CorruptNode
        );
    }

    obj->retain();

    return kernel::Expected<ShmObject*, ShmViewError>::success(obj);
}

ShmNodeView::ShmNodeView(ShmObject* obj)
    : obj_(obj) {
}

ShmNodeView::ShmNodeView(ShmNodeView&& other) noexcept
    : obj_(other.obj_) {
    other.obj_ = nullptr;
}

ShmNodeView& ShmNodeView::operator=(ShmNodeView&& other) noexcept {
    if (this == &other) {
        return *this;
    }

    shm_object_release(obj_);

    obj_ = other.obj_;
    other.obj_ = nullptr;

    return *this;
}

ShmNodeView::~ShmNodeView() {
    shm_object_release(obj_);
    obj_ = nullptr;
}

kernel::Expected<ShmNodeView, ShmViewError> ShmNodeView::from_node(vfs_node_t* node) {
    auto obj = shm_retain_object_from_node(node);

    if (!obj) {
        return kernel::Expected<ShmNodeView, ShmViewError>::failure(obj.error());
    }

    return kernel::Expected<ShmNodeView, ShmViewError>::success(
        ShmNodeView(obj.value())
    );
}

uint32_t ShmNodeView::size() const {
    return obj_ ? obj_->size() : 0u;
}

bool ShmNodeView::validate_range(uint32_t offset, uint32_t size_bytes) const {
    if (!obj_ || size_bytes == 0u) {
        return false;
    }

    const uint64_t off = (uint64_t)offset;
    const uint64_t end = off + (uint64_t)size_bytes;

    if (end < off) {
        return false;
    }

    return end <= (uint64_t)obj_->size();
}

bool ShmNodeView::phys_pages(const uint32_t*& out_pages, uint32_t& out_page_count) const {
    out_pages = nullptr;
    out_page_count = 0u;

    if (!obj_) {
        return false;
    }

    return obj_->get_phys_pages(out_pages, out_page_count);
}

static vfs_node_t* create_node_for_object(kernel::IntrusiveRef<ShmObject>&& obj) {
    if (!obj) {
        return nullptr;
    }

    vfs_node_t* node = (vfs_node_t*)kmalloc(sizeof(vfs_node_t));

    if (!node) {
        return nullptr;
    }

    ShmNodeData* data = new (kernel::nothrow) ShmNodeData(kernel::move(obj));

    if (!data) {
        kfree(node);

        return nullptr;
    }

    memset(node, 0, sizeof(*node));

    strlcpy(node->name, "shm", sizeof(node->name));

    node->flags = VFS_FLAG_SHM;
    node->size = data->obj->size();

    node->inode_idx = 0;
    node->refs = 1;

    node->private_data = data;
    node->private_release = [](void* p) {
        delete (ShmNodeData*)p;
    };

    return node;
}

static int shm_read(vfs_node_t* node, uint32_t offset, uint32_t size, void* buffer) {
    (void)node;
    (void)offset;
    (void)size;
    (void)buffer;

    return -1;
}

static int shm_write(vfs_node_t* node, uint32_t offset, uint32_t size, const void* buffer) {
    (void)node;
    (void)offset;
    (void)size;
    (void)buffer;

    return -1;
}

static int shm_close(vfs_node_t* node) {
    if (!node) {
        return -1;
    }

    return 0;
}

static vfs_ops_t shm_ops = {
    .read = shm_read,
    .write = shm_write,
    .open = 0,
    .close = shm_close,
    .ioctl = 0,
};

}
}

extern "C" {

int shm_get_phys_pages(struct vfs_node* node, const uint32_t** out_pages, uint32_t* out_page_count) {
    if (!node || !out_pages || !out_page_count) {
        return 0;
    }

    *out_pages = nullptr;
    *out_page_count = 0u;

    if ((node->flags & VFS_FLAG_SHM) == 0) {
        return 0;
    }

    auto* data = (kernel::shm::ShmNodeData*)node->private_data;

    if (!data || !data->obj) {
        return 0;
    }

    const uint32_t* pages = nullptr;
    uint32_t page_count = 0u;

    if (!data->obj->get_phys_pages(pages, page_count)) {
        return 0;
    }

    *out_pages = pages;
    *out_page_count = page_count;

    return 1;
}

struct vfs_node* shm_create_node(uint32_t size) {
    auto obj = kernel::shm::ShmObject::create(size);

    if (!obj) {
        return nullptr;
    }

    vfs_node_t* node = kernel::shm::create_node_for_object(kernel::move(obj));

    if (!node) {
        return nullptr;
    }

    node->ops = &kernel::shm::shm_ops;

    return node;
}

struct vfs_node* shm_create_named_node(const char* name, uint32_t size) {
    const uint32_t nlen = kernel::shm::name_len_bounded(name);

    if (nlen == 0u) {
        return nullptr;
    }

    if (name[nlen] != '\0') {
        return nullptr;
    }

    auto obj = kernel::shm::ShmObject::create(size);

    if (!obj) {
        return nullptr;
    }

    const kernel::string key(name);

    if (!kernel::shm::registry().insert_unique(key, obj.get())) {
        return nullptr;
    }

    vfs_node_t* node = kernel::shm::create_node_for_object(kernel::move(obj));

    if (!node) {
        (void)shm_unlink_named(name);

        return nullptr;
    }

    node->ops = &kernel::shm::shm_ops;

    return node;
}

struct vfs_node* shm_open_named_node(const char* name) {
    const uint32_t nlen = kernel::shm::name_len_bounded(name);

    if (nlen == 0u) {
        return nullptr;
    }

    if (name[nlen] != '\0') {
        return nullptr;
    }

    const kernel::string key(name);

    auto obj = kernel::shm::registry().find_and_retain(key);

    if (!obj) {
        return nullptr;
    }

    vfs_node_t* node = kernel::shm::create_node_for_object(kernel::move(obj));

    if (!node) {
        return nullptr;
    }

    node->ops = &kernel::shm::shm_ops;

    return node;
}

int shm_unlink_named(const char* name) {
    const uint32_t nlen = kernel::shm::name_len_bounded(name);

    if (nlen == 0u) {
        return -1;
    }

    if (name[nlen] != '\0') {
        return -1;
    }

    const kernel::string key(name);

    auto obj = kernel::shm::registry().remove(key);

    if (!obj) {
        return -1;
    }

    return 0;
}

}
