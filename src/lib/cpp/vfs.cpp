#include <lib/cpp/vfs.h>

#include <fs/pipe.h>

namespace kernel {

VirtualFSNode::VirtualFSNode() = default;

VirtualFSNode::VirtualFSNode(vfs_node_t* node)
    : node_(node) {
}

VirtualFSNode VirtualFSNode::from_borrowed(vfs_node_t* node) {
    if (node) {
        vfs_node_retain(node);
    }

    return VirtualFSNode(node);
}

VirtualFSNode::VirtualFSNode(VirtualFSNode&& other) noexcept
    : node_(other.node_) {
    other.node_ = nullptr;
}

VirtualFSNode& VirtualFSNode::operator=(VirtualFSNode&& other) noexcept {
    if (this == &other) {
        return *this;
    }

    reset();

    node_ = other.node_;
    other.node_ = nullptr;

    return *this;
}

VirtualFSNode::~VirtualFSNode() {
    reset();
}

void VirtualFSNode::reset(vfs_node_t* node) {
    if (node_) {
        vfs_node_release(node_);
    }

    node_ = node;
}

VirtualFSNode VirtualFSNode::retain() const {
    return from_borrowed(node_);
}

int VirtualFSNode::open() const {
    if (!node_ || !node_->ops || !node_->ops->open) {
        return -1;
    }

    return node_->ops->open(node_);
}

int VirtualFSNode::close() const {
    if (!node_ || !node_->ops || !node_->ops->close) {
        return -1;
    }

    return node_->ops->close(node_);
}

int VirtualFSNode::read(uint32_t offset, uint32_t size, void* buffer) const {
    if (!node_ || !node_->ops || !node_->ops->read) {
        return -1;
    }

    return node_->ops->read(node_, offset, size, buffer);
}

int VirtualFSNode::write(uint32_t offset, uint32_t size, const void* buffer) const {
    if (!node_ || !node_->ops || !node_->ops->write) {
        return -1;
    }

    return node_->ops->write(node_, offset, size, buffer);
}

int VirtualFSNode::ioctl(uint32_t req, void* arg) const {
    if (!node_ || !node_->ops || !node_->ops->ioctl) {
        return -1;
    }

    return node_->ops->ioctl(node_, req, arg);
}

VirtualFSPipe create_pipe() {
    vfs_node_t* read_node = nullptr;
    vfs_node_t* write_node = nullptr;

    if (vfs_create_pipe(&read_node, &write_node) != 0) {
        return {};
    }

    VirtualFSPipe pipe;

    pipe.read.reset(read_node);
    pipe.write.reset(write_node);

    return pipe;
}

}
