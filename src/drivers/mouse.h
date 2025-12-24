#ifndef DRIVERS_MOUSE_H
#define DRIVERS_MOUSE_H

#include <stdint.h>

#include <arch/i386/idt.h>

extern int mouse_x, mouse_y;
extern int mouse_buttons;

void mouse_init(void);
void mouse_handler(void);
void mouse_process_byte(uint8_t data);
void mouse_irq_handler(registers_t* regs);

void mouse_wait(uint8_t type);
void mouse_write(uint8_t a);

#endif