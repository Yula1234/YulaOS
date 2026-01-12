// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#ifndef DRIVERS_KEYBOARD_H
#define DRIVERS_KEYBOARD_H

#include <stdint.h>

#include <arch/i386/idt.h>

void kbd_init(void);
void kbd_handle_scancode(uint8_t scancode);
void keyboard_irq_handler(registers_t* regs);
int  kbd_try_read_char(char* out);

void kbd_vfs_init();

void kbd_reboot(void);

#endif