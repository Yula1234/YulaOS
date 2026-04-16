// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#ifndef YOS_SYSCALL_H
#define YOS_SYSCALL_H

static inline int syscall(int num, int arg1, int arg2, int arg3) {
    int ret;

    __asm__ volatile(
        "movl %%esp, %%ecx \n\t"
        "call 1f \n\t"
        "1: popl %%edx \n\t"             
        "addl $(2f - 1b), %%edx \n\t"
        "sysenter \n\t"
        "2: \n\t"
        : "=a"(ret)
        : "a"(num), "b"(arg1), "D"(arg2), "S"(arg3)
        : "memory", "ecx", "edx"
    );
    
    return ret;
}

static inline int syscall4(int num, int arg1, int arg2, int arg3, int arg4) {
    int ret;
    
    int dummy_esi = arg3;

    __asm__ volatile(
        "movl %[a4], %%ecx \n\t"
        "pushl %%ebp \n\t"
        "movl %%ecx, %%ebp \n\t"
        "movl %%esp, %%ecx \n\t"
        "call 1f \n\t"
        "1: popl %%edx \n\t"
        "addl $(2f - 1b), %%edx \n\t"
        "sysenter \n\t"
        "2: \n\t"
        "popl %%ebp \n\t"
        : "=a"(ret), "+S"(dummy_esi)
        : "a"(num), "b"(arg1), "D"(arg2), [a4] "rm"(arg4)
        : "memory", "ecx", "edx"
    );

    return ret;
}

#endif