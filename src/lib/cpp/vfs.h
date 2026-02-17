#ifndef LIB_CPP_VFS_H
#define LIB_CPP_VFS_H

#include <fs/vfs.h>

#include <stdint.h>

namespace kernel {

class VirtualFSNode {
public:
    VirtualFSNode();
    explicit VirtualFSNode(vfs_node_t* node);
    static VirtualFSNode from_borrowed(vfs_node_t* node);

    VirtualFSNode(const VirtualFSNode&) = delete;
    VirtualFSNode& operator=(const VirtualFSNode&) = delete;
    VirtualFSNode(VirtualFSNode&& other) noexcept;
    VirtualFSNode& operator=(VirtualFSNode&& other) noexcept;

    ~VirtualFSNode();

    vfs_node_t* get() const {
        return node_;
    }

    explicit operator bool() const {
        return node_ != nullptr;
    }

    vfs_node_t* release() {
        vfs_node_t* out = node_;
        node_ = nullptr;
        return out;
    }

    void reset(vfs_node_t* node = nullptr);

    VirtualFSNode retain() const;

    const char* name() const {
        return node_ ? node_->name : "";
    }

    uint32_t flags() const {
        return node_ ? node_->flags : 0u;
    }

    uint32_t size() const {
        return node_ ? node_->size : 0u;
    }

    uint32_t inode() const {
        return node_ ? node_->inode_idx : 0u;
    }

    int open() const;
    int close() const;
    int read(uint32_t offset, uint32_t size, void* buffer) const;
    int write(uint32_t offset, uint32_t size, const void* buffer) const;
    int ioctl(uint32_t req, void* arg) const;

private:
    vfs_node_t* node_ = nullptr;
};

struct VirtualFSPipe {
    VirtualFSNode read;
    VirtualFSNode write;

    explicit operator bool() const {
        return (bool)read && (bool)write;
    }
};

VirtualFSPipe create_pipe();

}

#endif
