#include "net_core.h"

#include <yula.h>

namespace netd {

void UniqueFd::reset(int v) {
    if (fd >= 0) {
        close(fd);
    }

    fd = v;
}

}
