// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#include <yula.h>

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("Usage: kill <pid>\n");
        return 1;
    }

    char* end = 0;
    long pid_l = strtol(argv[1], &end, 10);
    if (!end || *end != '\0' || pid_l <= 0 || pid_l > 0x7FFFFFFF) {
        printf("kill: invalid pid\n");
        return 1;
    }

    int rc = kill((int)pid_l);
    if (rc != 0) {
        printf("kill: failed\n");
        return 1;
    }

    return 0;
}
