#ifndef KERNEL_PANIC_H
#define KERNEL_PANIC_H

#include <stdint.h>

void kernel_panic(const char* message, const char* file, uint32_t line, registers_t* regs);

#endif
