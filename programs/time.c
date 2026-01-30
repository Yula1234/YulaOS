// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#include <yula.h>

static int spawn_by_name(const char* name, int argc, char** argv) {
    if (!name || !*name) return -1;

    if (name[0] == '/') {
        return spawn_process(name, argc, argv);
    }

    const size_t n = strlen(name);
    const int has_exe = (n >= 4 && strcmp(name + (n - 4u), ".exe") == 0);

    char path1[128];
    char path2[128];
    if (has_exe) {
        (void)snprintf(path1, sizeof(path1), "/bin/%s", name);
        (void)snprintf(path2, sizeof(path2), "/bin/usr/%s", name);
    } else {
        (void)snprintf(path1, sizeof(path1), "/bin/%s.exe", name);
        (void)snprintf(path2, sizeof(path2), "/bin/usr/%s.exe", name);
    }

    int pid = spawn_process(path1, argc, argv);
    if (pid < 0) pid = spawn_process(path2, argc, argv);
    return pid;
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
