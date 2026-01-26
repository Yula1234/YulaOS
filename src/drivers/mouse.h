// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#ifndef DRIVERS_MOUSE_H
#define DRIVERS_MOUSE_H

#include <stdint.h>

#include <kernel/poll_waitq.h>

#include <arch/i386/idt.h>

struct task;

typedef struct {
    int32_t x;
    int32_t y;
    int32_t buttons;
} mouse_state_t;

extern int mouse_x, mouse_y;
extern int mouse_buttons;

void mouse_init(void);
void mouse_handler(void);
void mouse_process_byte(uint8_t data);
void mouse_irq_handler(registers_t* regs);

void mouse_wait(uint8_t type);
void mouse_write(uint8_t a);

void mouse_vfs_init(void);

int mouse_poll_ready(struct task* task);
int mouse_poll_waitq_register(poll_waiter_t* w, struct task* task);
void mouse_poll_notify_focus_change(void);

#endif