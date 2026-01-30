// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#include <yula.h>

static uint32_t name_color(const char* name, uint32_t type) {
    if (type == 2) return 0x569CD6u;

    if (name) {
        size_t n = strlen(name);
        if (n >= 4 && strcmp(name + (n - 4u), ".exe") == 0) return 0xB5CEA8u;
        if (n >= 4 && strcmp(name + (n - 4u), ".asm") == 0) return 0xCE9178u;
        if (n >= 2 && strcmp(name + (n - 2u), ".c") == 0) return 0xCE9178u;
    }

    return 0xD4D4D4u;
}

int main(int argc, char** argv) {
    const char* path = (argc > 1) ? argv[1] : ".";

    int fd = open(path, 0);
    if (fd < 0) {
        printf("ls: cannot open '%s'\n", path);
        return 1;
    }

    const uint32_t C_BG = 0x141414u;

    yfs_dirent_info_t dents[64];
    for (;;) {
        int n = getdents(fd, dents, (uint32_t)sizeof(dents));
        if (n < 0) {
            printf("ls: getdents failed\n");
            close(fd);
            return 1;
        }
        if (n == 0) break;

        int cnt = n / (int)sizeof(yfs_dirent_info_t);
        for (int i = 0; i < cnt; i++) {
            yfs_dirent_info_t* d = &dents[i];
            if (d->inode == 0) continue;
            if (strcmp(d->name, ".") == 0 || strcmp(d->name, "..") == 0) continue;

            uint32_t fg = name_color(d->name, d->type);
            set_console_color(fg, C_BG);

            print(d->name);
            if (d->type == 2) print("/");
            print("\n");
        }
    }

    close(fd);
    set_console_color(0xD4D4D4u, C_BG);
    return 0;
}
