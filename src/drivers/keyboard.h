#ifndef DRIVERS_KEYBOARD_H
#define DRIVERS_KEYBOARD_H

#include <stdint.h>

#include <arch/i386/idt.h>

void kbd_init(void);
void keyboard_irq_handler(registers_t* regs);
int  kbd_try_read_char(char* out);

void kbd_vfs_init();

void kbd_reboot(void);

#endif