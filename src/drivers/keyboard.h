// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#ifndef DRIVERS_KEYBOARD_H
#define DRIVERS_KEYBOARD_H

#include <stdint.h>

#include <kernel/poll_waitq.h>

#include <arch/i386/idt.h>

struct task;

void kbd_init(void);
void kbd_handle_scancode(uint8_t scancode);
void keyboard_irq_handler(registers_t* regs);
int  kbd_try_read_char(char* out);

void kbd_vfs_init();

void kbd_reboot(void);

int  kbd_poll_ready(struct task* task);
int  kbd_poll_waitq_register(poll_waiter_t* w, struct task* task);
void kbd_poll_notify_focus_change(void);

#endif