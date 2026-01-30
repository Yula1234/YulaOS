// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#include <yula.h>

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("Usage: rm <path>\n");
        return 1;
    }

    int rc = unlink(argv[1]);
    if (rc != 0) {
        printf("rm: failed\n");
        return 1;
    }

    return 0;
}
