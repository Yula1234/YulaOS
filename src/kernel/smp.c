#include <stdint.h>
#include <lib/string.h>
#include <hal/apic.h>
#include <hal/io.h>
#include <hal/simd.h>
#include <mm/heap.h>
#include <mm/pmm.h>
#include <arch/i386/gdt.h>
#include <arch/i386/idt.h>
#include <arch/i386/paging.h>
#include <drivers/vga.h>
#include <kernel/sched.h>
#include "cpu.h"

extern uint8_t smp_trampoline_start[];
extern uint8_t smp_trampoline_end[];

volatile int ap_running_count = 0;

void smp_ap_main(cpu_t* cpu_arg) {
    cpu_t* cpu = cpu_arg; 
    cpu->started = 1;

    gdt_load();
    idt_load();
    
    int tss_selector = (5 + cpu->index) * 8;
    __asm__ volatile("ltr %%ax" : : "a" (tss_selector));
    
    paging_switch(kernel_page_directory);
    
    lapic_init();
    lapic_timer_init(15000);
    kernel_init_simd();
    
    __sync_fetch_and_add(&ap_running_count, 1);
    
    sched_yield();
}

static void mdulay(int ms) {
    for (volatile int i = 0; i < ms * 10000; i++);
}

void smp_boot_aps(void) {
    uint32_t size = smp_trampoline_end - smp_trampoline_start;
    if (size > 4096) return;
    
    memcpy((void*)0x1000, smp_trampoline_start, size);

    volatile uint32_t* tramp_stack = (volatile uint32_t*)(0x1000 + 4);
    volatile uint32_t* tramp_code  = (volatile uint32_t*)(0x1000 + 8);
    volatile uint32_t* tramp_cr3   = (volatile uint32_t*)(0x1000 + 12);
    volatile uint32_t* tramp_arg   = (volatile uint32_t*)(0x1000 + 16);

    *tramp_code = (uint32_t)smp_ap_main;
    *tramp_cr3  = (uint32_t)kernel_page_directory;

    cpu_t* bsp = cpu_current();

    for (int i = 0; i < cpu_count; i++) {
        if (cpus[i].id == bsp->id) continue;

        void* stack = kmalloc_a(4096);
        *tramp_stack = (uint32_t)stack + 4096;
        
        *tramp_arg = (uint32_t)&cpus[i];

        cpus[i].started = 0;

        lapic_write(LAPIC_ICRHI, cpus[i].id << 24);
        lapic_write(LAPIC_ICRLO, 0x00004500);
        mdulay(10);

        lapic_write(LAPIC_ICRHI, cpus[i].id << 24);
        lapic_write(LAPIC_ICRLO, 0x00004601);
        mdulay(1);

        lapic_write(LAPIC_ICRHI, cpus[i].id << 24);
        lapic_write(LAPIC_ICRLO, 0x00004601);
        mdulay(100);
    }
}