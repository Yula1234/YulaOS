// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#ifndef KERNEL_PANIC_H
#define KERNEL_PANIC_H

#include <stdint.h>
#include <arch/i386/idt.h>

void kernel_panic(const char* message, const char* file, uint32_t line, registers_t* regs);

#define panic(msg) kernel_panic(msg, __FILE__, __LINE__, 0)

#endif
