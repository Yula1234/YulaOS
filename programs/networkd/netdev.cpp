#include "netdev.h"

#include <yula.h>

namespace netd {

NetDev::NetDev() : m_fd(), m_mac{} {
}

bool NetDev::open_default() {
    m_fd.reset(open("/dev/ne2k0", 0));
    if (m_fd.get() < 0) {
        return false;
    }

    yos_net_mac_t mac{};
    if (ioctl(m_fd.get(), YOS_NET_GET_MAC, &mac) != 0) {
        return false;
    }

    for (int i = 0; i < 6; i++) {
        m_mac.b[i] = mac.mac[i];
    }

    return true;
}

int NetDev::fd() const {
    return m_fd.get();
}

Mac NetDev::mac() const {
    return m_mac;
}

int NetDev::read_frame(uint8_t* out, uint32_t cap) {
    if (m_fd.get() < 0 || !out || cap == 0) {
        return -1;
    }

    return read(m_fd.get(), out, cap);
}

int NetDev::write_frame(const uint8_t* data, uint32_t len) {
    if (m_fd.get() < 0 || !data || len == 0) {
        return -1;
    }

    static constexpr uint32_t kEthMinFrameNoFcs = 60u;
    if (len < kEthMinFrameNoFcs) {
        uint8_t buf[kEthMinFrameNoFcs];

        memcpy(buf, data, len);
        memset(buf + len, 0, kEthMinFrameNoFcs - len);

        return write(m_fd.get(), buf, kEthMinFrameNoFcs);
    }

    return write(m_fd.get(), data, len);
}

}
