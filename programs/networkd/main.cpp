#include "netd_app.h"

extern "C" int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    netd::NetdApp app;
    if (!app.init()) {
        printf("networkd: init failed\n");
        return 1;
    }

    return app.run();
}
