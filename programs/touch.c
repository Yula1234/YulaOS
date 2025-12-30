// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#include <yula.h>

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("Usage: touch <file>...\n");
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
                set_console_color(0xF44747, 0x141414);
                printf("touch: cannot create '%s'\n", path);
                set_console_color(0xD4D4D4, 0x141414);
            }
        }
    }

    return 0;
}