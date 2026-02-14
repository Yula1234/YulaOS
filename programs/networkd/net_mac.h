#ifndef YOS_NETD_NET_MAC_H
#define YOS_NETD_NET_MAC_H

#include "net_proto.h"

namespace netd {

inline Mac mac_from_bytes(const uint8_t b[6]) {
    Mac m{};

    for (int i = 0; i < 6; i++) {
        m.b[i] = b[i];
    }

    return m;
}

inline void mac_to_bytes(const Mac& m, uint8_t out[6]) {
    for (int i = 0; i < 6; i++) {
        out[i] = m.b[i];
    }
}

}

#endif
