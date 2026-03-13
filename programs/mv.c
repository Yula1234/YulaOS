// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#include <yula.h>

int main(int argc, char** argv) {
    if (argc < 3) {
        printf("Usage: mv <source> <destination>\n");
        return 1;
    }

    const char* src = argv[1];
    const char* dest = argv[2];

    if (rename(src, dest) == 0) {
        return 0;
    } else {
        puts("\x1b[91m");
        printf("mv: failed to rename '%s' to '%s'\n", src, dest);
        puts("\x1b[0m");
        return 1;
    }
}