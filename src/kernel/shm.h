// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 Yula1234

#ifndef KERNEL_SHM_H
#define KERNEL_SHM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct vfs_node;

struct vfs_node* shm_create_node(uint32_t size);

struct vfs_node* shm_create_named_node(const char* name, uint32_t size);

struct vfs_node* shm_open_named_node(const char* name);

int shm_unlink_named(const char* name);

int shm_get_phys_pages(struct vfs_node* node, const uint32_t** out_pages, uint32_t* out_page_count);

#ifdef __cplusplus
}

#include <lib/cpp/expected.h>

namespace kernel {
namespace shm {

struct ShmObject;

enum class ShmViewError : uint32_t {
    InvalidArg,
    NotShmNode,
    CorruptNode,
    RangeError,
};

kernel::Expected<ShmObject*, ShmViewError> shm_retain_object_from_node(struct vfs_node* node);

void shm_object_retain(ShmObject* obj);

void shm_object_release(ShmObject* obj);

class ShmNodeView {
public:
    static kernel::Expected<ShmNodeView, ShmViewError> from_node(struct vfs_node* node);

    ShmNodeView(const ShmNodeView&) = delete;
    ShmNodeView& operator=(const ShmNodeView&) = delete;

    ShmNodeView(ShmNodeView&& other) noexcept;
    ShmNodeView& operator=(ShmNodeView&& other) noexcept;

    ~ShmNodeView();

    uint32_t size() const;

    bool validate_range(uint32_t offset, uint32_t size_bytes) const;
    bool phys_pages(const uint32_t*& out_pages, uint32_t& out_page_count) const;

private:
    explicit ShmNodeView(ShmObject* obj);

private:
    ShmObject* obj_ = nullptr;
};

}
}
#endif

#endif
