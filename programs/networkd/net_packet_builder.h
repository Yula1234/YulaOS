#ifndef YOS_NETD_PACKET_BUILDER_H
#define YOS_NETD_PACKET_BUILDER_H

#include <stdint.h>

namespace netd {

class PacketBuilder {
public:
    static constexpr uint32_t kCap = 1600u;
    static constexpr uint32_t kDefaultHeadroom = 128u;

    PacketBuilder();

    PacketBuilder(const PacketBuilder&) = delete;
    PacketBuilder& operator=(const PacketBuilder&) = delete;

    void reset();

    uint8_t* prepend(uint32_t n);

    uint8_t* append(uint32_t n);

    bool append_copy(const void* src, uint32_t n);

    uint8_t* data();

    const uint8_t* data() const;

    uint32_t size() const;

    uint32_t capacity() const;

private:
    uint8_t m_buf[kCap];
    uint32_t m_begin;
    uint32_t m_end;
};

}

#endif
