#ifndef KERNEL_SYSCALL_H
#define KERNEL_SYSCALL_H

#include <arch/i386/idt.h>

void syscall_handler(registers_t* regs);

#endif