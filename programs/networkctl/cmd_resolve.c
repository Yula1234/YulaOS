// SPDX-License-Identifier: GPL-2.0

#include "netctl_cmd.h"

#include "netctl_fmt.h"
#include "netctl_ipc.h"
#include "netctl_parse.h"
#include "netctl_print.h"

static int netctl_parse_resolve_args(int argc, char** argv, const char** out_name, uint32_t* out_timeout_ms) {
    if (!out_name || !out_timeout_ms) {
        return -1;
    }

    if (argc < 1) {
        return -1;
    }

    const char* name = argv[0];
    uint32_t timeout_ms = 1000;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
            uint32_t tmp = 0;
            if (!netctl_parse_u32(argv[++i], &tmp) || tmp == 0) {
                return -1;
            }
            timeout_ms = tmp;
            continue;
        }

        if (argv[i][0] == '-' && argv[i][1] == 't' && argv[i][2] != '\0') {
            uint32_t tmp = 0;
            if (!netctl_parse_u32(argv[i] + 2, &tmp) || tmp == 0) {
                return -1;
            }
            timeout_ms = tmp;
            continue;
        }

        return -1;
    }

    *out_name = name;
    *out_timeout_ms = timeout_ms;
    return 0;
}

int netctl_cmd_resolve(int argc, char** argv) {
    const char* name = 0;
    uint32_t timeout_ms = 0;
    if (netctl_parse_resolve_args(argc, argv, &name, &timeout_ms) != 0) {
        netctl_print_usage();
        return 1;
    }

    netctl_session_t s;
    if (netctl_session_open(&s) != 0) {
        printf("networkctl: cannot connect to networkd\n");
        return 1;
    }

    if (netctl_session_send_hello(&s) != 0) {
        netctl_session_close(&s);
        printf("networkctl: cannot connect to networkd\n");
        return 1;
    }

    uint32_t addr = 0;
    if (netctl_dns_query(&s, name, timeout_ms, &addr) != 0) {
        printf("resolve: failed\n");
        netctl_session_close(&s);
        return 1;
    }

    char ip[32];
    netctl_ip4_to_str(addr, ip, (uint32_t)sizeof(ip));
    printf("%s -> %s\n", name, ip);

    netctl_session_close(&s);
    return 0;
}

