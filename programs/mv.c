// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#include <yula.h>

int main(int argc, char** argv) {
    if (argc < 3) {
        printf("Usage: mv <old_path> <new_path>\n");
        return 1;
    }

    const char* src = argv[1];
    const char* dest = argv[2];

    if (rename(src, dest) == 0) {
        return 0;
    } else {
        set_console_color(0xF44747, 0x141414);
        printf("mv: failed to rename '%s' to '%s'\n", src, dest);
        set_console_color(0xD4D4D4, 0x141414);
        return 1;
    }
}