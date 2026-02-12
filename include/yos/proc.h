// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#ifndef YOS_PROC_H
#define YOS_PROC_H

#include <stdint.h>

#define YOS_PROC_NAME_MAX 32

typedef struct {
    uint32_t pid;
    uint32_t parent_pid;
    uint32_t state;
    uint32_t priority;
    uint32_t mem_pages;
    uint32_t term_mode;
    char name[YOS_PROC_NAME_MAX];
} __attribute__((packed)) yos_proc_info_t;

#define YOS_SYS_CLONE 20

typedef void (*yos_thread_fn_t)(void*);

static inline int yos_clone(yos_thread_fn_t entry, void* arg, void* stack_top, uint32_t stack_size) {
    int res = -1;
    __asm__ volatile(
        "int $0x80"
        : "=a"(res)
        : "a"(YOS_SYS_CLONE), "b"(entry), "c"(arg), "d"(stack_top), "S"(stack_size)
        : "memory"
    );
    return res;
}

#endif
