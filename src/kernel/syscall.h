// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#ifndef KERNEL_SYSCALL_H
#define KERNEL_SYSCALL_H

#include <arch/i386/idt.h>

#ifdef __cplusplus
extern "C" {
#endif

void syscall_handler(registers_t* regs);

#ifdef __cplusplus
}
#endif

#endif