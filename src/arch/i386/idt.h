#ifndef ARCH_I386_IDT_H
#define ARCH_I386_IDT_H

#include <stdint.h>

    
typedef struct {
    uint32_t gs, fs, es, ds; 
    uint32_t edi, esi, ebp, esp, ebx, edx, ecx, eax; 
    uint32_t int_no, err_code; 
    uint32_t eip, cs, eflags, useresp, ss;
} __attribute__((packed)) registers_t;


extern volatile uint32_t system_uptime_seconds;
  

void idt_init(void);

#endif