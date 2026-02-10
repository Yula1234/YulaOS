// SPDX-License-Identifier: GPL-2.0

#include "netctl_cmd.h"

#include "netctl_print.h"
#include "netctl_proc.h"

static int netctl_daemon_status(void) {
    yos_proc_info_t info;
    int running = netctl_find_process("networkd", &info);

    if (!running) {
        printf("daemon: stopped\n");
        return 0;
    }

    printf("daemon: running\n");
    printf("pid: %u\n", info.pid);
    printf("state: %s\n", netctl_proc_state_name(info.state));
    return 0;
}

static int netctl_daemon_stop(void) {
    yos_proc_info_t info;
    int running = netctl_find_process("networkd", &info);

    if (!running) {
        printf("daemon: already stopped\n");
        return 0;
    }

    if (kill((int)info.pid) != 0) {
        printf("daemon: kill failed\n");
        return 1;
    }

    printf("daemon: stopped\n");
    return 0;
}

static int netctl_daemon_start(void) {
    yos_proc_info_t info;
    int running = netctl_find_process("networkd", &info);

    if (running) {
        printf("daemon: already running (pid %u)\n", info.pid);
        return 0;
    }

    char* args[2];
    args[0] = (char*)"networkd";
    args[1] = 0;

    int pid = spawn_process_resolved("networkd", 1, args);
    if (pid < 0) {
        printf("daemon: spawn failed\n");
        return 1;
    }

    printf("daemon: started (pid %d)\n", pid);
    return 0;
}

static int netctl_daemon_restart(void) {
    yos_proc_info_t info;
    int running = netctl_find_process("networkd", &info);

    if (running) {
        (void)kill((int)info.pid);
        sleep(50);
    }

    char* args[2];
    args[0] = (char*)"networkd";
    args[1] = 0;

    int pid = spawn_process_resolved("networkd", 1, args);
    if (pid < 0) {
        printf("daemon: spawn failed\n");
        return 1;
    }

    printf("daemon: restarted (pid %d)\n", pid);
    return 0;
}

int netctl_cmd_daemon(int argc, char** argv) {
    const char* sub = "status";
    if (argc >= 1) {
        sub = argv[0];
    }

    if (strcmp(sub, "status") == 0) {
        return netctl_daemon_status();
    }

    if (strcmp(sub, "stop") == 0) {
        return netctl_daemon_stop();
    }

    if (strcmp(sub, "start") == 0) {
        return netctl_daemon_start();
    }

    if (strcmp(sub, "restart") == 0) {
        return netctl_daemon_restart();
    }

    netctl_print_usage();
    return 1;
}

