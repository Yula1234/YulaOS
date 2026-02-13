#ifndef YOS_NETD_U32_MAP_H
#define YOS_NETD_U32_MAP_H

#include "net_hash_map.h"

#include <stdint.h>

namespace netd {

using U32Map = HashMap<uint32_t, uint32_t>;

}

#endif
