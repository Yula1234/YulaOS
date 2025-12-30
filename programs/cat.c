// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#include <yula.h>

#define BUF_SIZE 1024

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("Usage: cat <filename>\n");
        return 1;
    }

    int fd = open(argv[1], 0);
    if (fd < 0) {
        printf("cat: %s: No such file or directory\n", argv[1]);
        return 1;
    }

    char buf[BUF_SIZE];
    int n;

    while ((n = read(fd, buf, BUF_SIZE)) > 0) {
        write(1, buf, n);
    }

    close(fd);
    
    print("\n");
    
    return 0;
}