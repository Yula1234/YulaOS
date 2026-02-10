// SPDX-License-Identifier: GPL-2.0

#include "netctl_common.h"

#include "netctl_cmd.h"
#include "netctl_print.h"

int main(int argc, char** argv) {
    if (argc <= 1) {
        return netctl_cmd_status(1);
    }

    const char* cmd = argv[1];

    if (strcmp(cmd, "-h") == 0 || strcmp(cmd, "--help") == 0 || strcmp(cmd, "help") == 0) {
        netctl_print_usage();
        return 0;
    }

    if (strcmp(cmd, "status") == 0) {
        return netctl_cmd_status(0);
    }

    if (strcmp(cmd, "links") == 0) {
        return netctl_cmd_links();
    }

    if (strcmp(cmd, "ping") == 0) {
        return netctl_cmd_ping(argc - 2, &argv[2]);
    }

    if (strcmp(cmd, "resolve") == 0) {
        return netctl_cmd_resolve(argc - 2, &argv[2]);
    }

    if (strcmp(cmd, "config") == 0) {
        return netctl_cmd_config(argc - 2, &argv[2]);
    }

    if (strcmp(cmd, "up") == 0) {
        return netctl_cmd_iface(1);
    }

    if (strcmp(cmd, "down") == 0) {
        return netctl_cmd_iface(0);
    }

    if (strcmp(cmd, "daemon") == 0) {
        return netctl_cmd_daemon(argc - 2, &argv[2]);
    }

    netctl_print_usage();
    return 1;
}

