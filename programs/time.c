// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#include <yula.h>

static int spawn_by_name(const char* name, int argc, char** argv) {
    if (!name || !*name) return -1;
    return spawn_process_resolved(name, argc, argv);
}

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("Usage: time <command> [args...]\n");
        return 1;
    }

    uint32_t start = uptime_ms();

    int pid = spawn_by_name(argv[1], argc - 1, &argv[1]);
    if (pid < 0) {
        printf("time: spawn failed\n");
        return 1;
    }

    int st = 0;
    (void)waitpid(pid, &st);

    uint32_t end = uptime_ms();
    uint32_t diff = end - start;

    uint32_t sec = diff / 1000u;
    uint32_t ms = diff % 1000u;

    printf("real %u.%03u s\n", sec, ms);
    return 0;
}
