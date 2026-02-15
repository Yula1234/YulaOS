#include "net_packet_builder.h"

#include <yula.h>

namespace netd {

PacketBuilder::PacketBuilder() : m_buf{}, m_begin(0u), m_end(0u) {
    reset();
}

void PacketBuilder::reset() {
    m_begin = kDefaultHeadroom;
    m_end = kDefaultHeadroom;
}

uint8_t* PacketBuilder::prepend(uint32_t n) {
    if (n == 0u || n > m_begin) {
        return nullptr;
    }

    m_begin -= n;
    return m_buf + m_begin;
}

uint8_t* PacketBuilder::append(uint32_t n) {
    if (n == 0u || (kCap - m_end) < n) {
        return nullptr;
    }

    uint8_t* p = m_buf + m_end;
    m_end += n;
    return p;
}

bool PacketBuilder::append_copy(const void* src, uint32_t n) {
    if (n == 0u) {
        return true;
    }

    if (!src) {
        return false;
    }

    uint8_t* dst = append(n);
    if (!dst) {
        return false;
    }

    memcpy(dst, src, n);
    return true;
}

uint8_t* PacketBuilder::data() {
    return m_buf + m_begin;
}

const uint8_t* PacketBuilder::data() const {
    return m_buf + m_begin;
}

uint32_t PacketBuilder::size() const {
    return m_end - m_begin;
}

uint32_t PacketBuilder::capacity() const {
    return kCap;
}

}
