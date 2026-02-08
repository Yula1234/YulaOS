// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#include <arch/i386/idt.h>
#include <drivers/fbdev.h>

extern const uint8_t font8x16_basic[128][16];

static void panic_putc(int x, int y, char c) {
    if ((uint8_t)c > 127) return;
    const uint8_t* glyph = font8x16_basic[(uint8_t)c];
    for (int i = 0; i < 16; i++) {
        for (int j = 0; j < 8; j++) {
            if (glyph[i] & (1 << (7 - j))) {
                fb_ptr[(y + i) * fb_width + (x + j)] = 0xFFFFFFFF;
            }
        }
    }
}

static void panic_print(int x, int y, const char* s) {
    while (*s) {
        panic_putc(x, y, *s++);
        x += 9;
    }
}

static void panic_print_hex(int x, int y, uint32_t val) {
    const char* hex = "0123456789ABCDEF";
    char buf[11];
    buf[0] = '0'; buf[1] = 'x';
    for (int i = 0; i < 8; i++) {
        buf[9 - i] = hex[val & 0xF];
        val >>= 4;
    }
    buf[10] = 0; 
    panic_print(x, y, buf);
}

void kernel_panic(const char* message, const char* file, uint32_t line, registers_t* regs) {
    __asm__ volatile("cli");

    uint32_t total_pixels = fb_width * fb_height;
    for (uint32_t i = 0; i < total_pixels; i++) {
        fb_ptr[i] = 0xFF0000AA;
    }

    int y = 50;
    panic_print(50, y, "!!! YULAOS KERNEL PANIC !!!"); y += 30;
    
    panic_print(50, y, "Error: "); 
    panic_print(110, y, message); 
    y += 20;

    if (file) {
        panic_print(50, y, "File: "); panic_print(110, y, file); y += 20;
        panic_print(50, y, "Line: "); panic_print_hex(110, y, line); y += 30;
    }

    if (regs) {
        panic_print(50, y, "CPU Context:"); y += 20;
        
        panic_print(50, y, "EAX: "); panic_print_hex(90, y, regs->eax);
        panic_print(200, y, "EBX: "); panic_print_hex(240, y, regs->ebx);
        panic_print(350, y, "ECX: "); panic_print_hex(390, y, regs->ecx);
        panic_print(500, y, "EDX: "); panic_print_hex(540, y, regs->edx);
        y += 20;
        
        panic_print(50, y, "ESI: "); panic_print_hex(90, y, regs->esi);
        panic_print(200, y, "EDI: "); panic_print_hex(240, y, regs->edi);
        panic_print(350, y, "EBP: "); panic_print_hex(390, y, regs->ebp);
        panic_print(500, y, "ESP: "); panic_print_hex(540, y, regs->esp);
        y += 20;
        
        panic_print(50, y, "EIP: "); panic_print_hex(90, y, regs->eip);
        panic_print(200, y, "EFLAGS: "); panic_print_hex(280, y, regs->eflags);
        
        if (regs->int_no == 14) {
            uint32_t cr2;
            __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
            y += 30;
            panic_print(50, y, "PAGE FAULT ADDR (CR2): ");
            panic_print_hex(260, y, cr2);
        }
    }

    while(1) {
        __asm__ volatile("hlt");
    }
}
