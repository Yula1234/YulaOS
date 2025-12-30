// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#include <yula.h>

#define BUF_SIZE 4096

int main(int argc, char** argv) {
    if (argc < 3) {
        printf("Usage: cp <source> <dest>\n");
        return 1;
    }

    int fd_in = open(argv[1], 0); // 0 = Read mode
    if (fd_in < 0) {
        printf("cp: cannot open source file '%s'\n", argv[1]);
        return 1;
    }

    int fd_out = open(argv[2], 1); 
    if (fd_out < 0) {
        printf("cp: cannot create destination file '%s'\n", argv[2]);
        close(fd_in);
        return 1;
    }

    char* buf = malloc(BUF_SIZE);
    if (!buf) {
        printf("cp: out of memory\n");
        close(fd_in);
        close(fd_out);
        return 1;
    }

    int n_read;
    int total_bytes = 0;

    while ((n_read = read(fd_in, buf, BUF_SIZE)) > 0) {
        int n_written = write(fd_out, buf, n_read);
        if (n_written != n_read) {
            printf("cp: write error\n");
            break;
        }
        total_bytes += n_written;
    }

    free(buf);
    close(fd_in);
    close(fd_out);

    return 0;
}