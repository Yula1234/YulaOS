#ifndef YOS_NETD_NETDEV_H
#define YOS_NETD_NETDEV_H

#include "net_core.h"
#include "net_proto.h"

#include <stdint.h>

namespace netd {

class NetDev {
public:
    NetDev();

    NetDev(const NetDev&) = delete;
    NetDev& operator=(const NetDev&) = delete;

    bool open_default();

    int fd() const;
    Mac mac() const;

    int read_frame(uint8_t* out, uint32_t cap);
    int write_frame(const uint8_t* data, uint32_t len);

private:
    UniqueFd m_fd;
    Mac m_mac;
};

}

#endif
