// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#include <yula.h>

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("\x1b[91mUsage: touch <file>...\x1b[0m\n");
        return 1;
    }

    for (int i = 1; i < argc; i++) {
        const char* path = argv[i];

        int fd = open(path, 0);

        if (fd >= 0) {
            close(fd);
        } else {
            fd = open(path, 1);
            
            if (fd >= 0) {
                close(fd);
            } else {
                print("\x1b[91m");
                printf("touch: cannot create '%s'\n", path);
                print("\x1b[0m");
            }
        }
    }

    return 0;
}