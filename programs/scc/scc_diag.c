// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#include "scc_diag.h"

void scc_fatal_at(const char* file, const char* src, int line, int col, const char* msg) {
    puts("\x1b[91m");
    printf("\n[SCC ERROR] %s:%d:%d: %s\n", file ? file : "<input>", line, col, msg ? msg : "error");

    if (src) {
        const char* p = src;
        int cur = 1;
        while (*p && cur < line) {
            if (*p == '\n') cur++;
            p++;
        }

        const char* ls = p;
        while (*p && *p != '\n') p++;
        const char* le = p;

        printf("%.*s\n", (int)(le - ls), ls);
        for (int i = 1; i < col; i++) putchar(' ');
        printf("^\n");
    }

    puts("\x1b[0m");
    exit(1);
}
