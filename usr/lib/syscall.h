// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#ifndef YOS_SYSCALL_H
#define YOS_SYSCALL_H

static inline int syscall(int num, int arg1, int arg2, int arg3) {
    int ret;
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(num), "b"(arg1), "c"(arg2), "d"(arg3)
        : "memory", "cc" 
    );
    return ret;
}

#endif