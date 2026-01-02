// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#include <shell/shell.h>

#include <fs/yulafs.h>
#include <fs/bcache.h>

#include <drivers/pc_speaker.h>
#include <drivers/keyboard.h>
#include <drivers/console.h>
#include <drivers/mouse.h>
#include <drivers/ahci.h>
#include <drivers/acpi.h> 
#include <drivers/vga.h>

#include <kernel/clipboard.h>
#include <kernel/gui_task.h>
#include <kernel/window.h>
#include <kernel/sched.h>
#include <kernel/proc.h>
#include <kernel/cpu.h>

#include <arch/i386/paging.h>
#include <arch/i386/gdt.h>
#include <arch/i386/idt.h>

#include <hal/apic.h>
#include <hal/simd.h>
#include <hal/pit.h>
#include <hal/io.h>

#include <mm/heap.h>
#include <mm/pmm.h>
#include <kernel/panic.h>

#include <stdint.h>

typedef struct {
    uint32_t size;
    uint64_t addr;
    uint64_t len;
    uint32_t type;
} __attribute__((packed)) multiboot_memory_map_t;

typedef struct {
    uint32_t flags;

    uint32_t mem_lower;
    uint32_t mem_upper;

    uint32_t boot_device;
    uint32_t cmdline;

    uint32_t mods_count;
    uint32_t mods_addr;

    uint32_t unused[4];

    uint32_t mmap_length;
    uint32_t mmap_addr;

    uint32_t drives_length;
    uint32_t drives_addr;

    uint32_t config_table;
    uint32_t boot_loader_name;
    uint32_t apm_table;

    uint32_t vbe_control_info;
    uint32_t vbe_mode_info;
    uint16_t vbe_mode;
    uint16_t vbe_interface_seg;
    uint16_t vbe_interface_off;
    uint16_t vbe_interface_len;

    uint64_t framebuffer_addr;
    uint32_t framebuffer_pitch;
    uint32_t framebuffer_width;
    uint32_t framebuffer_height;
    uint8_t  framebuffer_bpp;
    uint8_t  framebuffer_type;
} __attribute__((packed)) multiboot_info_t;

uint32_t* fb_ptr;
uint32_t  fb_width;
uint32_t  fb_height;
uint32_t  fb_pitch;

extern uint32_t kernel_end; 

extern void put_pixel(int x, int y, uint32_t color);

extern void smp_boot_aps(void);

static inline void sys_usleep(uint32_t us) {
    __asm__ volatile("int $0x80" : : "a"(11), "b"(us));
}

void idle_task_func(void* arg) {
    (void)arg;
    while(1) {
        __asm__ volatile("sti");
        cpu_hlt();
        sched_yield();
    }
}

void syncer_task(void* arg) {
    (void)arg;
    while(1) {
        sys_usleep(400000); 
        
        bcache_sync();
    }
}

__attribute__((target("no-sse"))) void kmain(uint32_t magic, multiboot_info_t* mb_info) {
    if (magic != 0x2BADB002) {
        registers_t dummy_regs = {0};
        kernel_panic("Invalid multiboot magic number", __FILE__, __LINE__, &dummy_regs);
    }

    if (!(mb_info->flags & (1u << 12))) {
        registers_t dummy_regs = {0};
        kernel_panic("Bootloader did not provide framebuffer info", __FILE__, __LINE__, &dummy_regs);
    }

    if (mb_info->framebuffer_type != 1 || mb_info->framebuffer_bpp != 32) {
        registers_t dummy_regs = {0};
        kernel_panic("Unsupported framebuffer mode (need 32bpp linear)", __FILE__, __LINE__, &dummy_regs);
    }

    if (mb_info->framebuffer_width == 0 || mb_info->framebuffer_height == 0 ||
        mb_info->framebuffer_pitch < mb_info->framebuffer_width * 4) {
        registers_t dummy_regs = {0};
        kernel_panic("Invalid framebuffer parameters", __FILE__, __LINE__, &dummy_regs);
    }

    kernel_init_simd(); 

    cpu_init_system();

    fb_ptr = (uint32_t*)(uint32_t)mb_info->framebuffer_addr;
    fb_width = mb_info->framebuffer_width;
    fb_height = mb_info->framebuffer_height;
    fb_pitch = mb_info->framebuffer_pitch;

    gdt_init();
    idt_init();


    lapic_init();
    lapic_timer_init(15000);

    uint32_t memory_end_addr = 0;

    if (mb_info->flags & (1 << 6)) {
        multiboot_memory_map_t* mmap = (multiboot_memory_map_t*)mb_info->mmap_addr;
        
        while((uint32_t)mmap < mb_info->mmap_addr + mb_info->mmap_length) {
            if (mmap->type == 1) {
                uint64_t end = mmap->addr + mmap->len;
                if (end > memory_end_addr) {
                    memory_end_addr = (uint32_t)end;
                }
            }
            mmap = (multiboot_memory_map_t*)((uint32_t)mmap + mmap->size + sizeof(uint32_t));
        }
    } 

    else if (mb_info->flags & (1 << 0)) {
        memory_end_addr = (mb_info->mem_upper * 1024) + 0x100000;
    }

    if (memory_end_addr == 0) {
        memory_end_addr = 1024 * 1024 * 64; 
    }
    
    #define PIC_MASTER_PORT 0x21
    #define PIC_SLAVE_PORT  0xA1
    #define PIC_MASK_TIMER   (1 << 0)
    #define PIC_MASK_KEYBOARD (1 << 1)
    #define PIC_MASK_CASCADE (1 << 2)
    #define PIC_MASK_MOUSE   (1 << 4)
    #define PIC_MASK_HDD     (1 << 6)
    
    // Master PIC: disable timer (using APIC), enable keyboard and cascade
    outb(PIC_MASTER_PORT, ~(PIC_MASK_KEYBOARD | PIC_MASK_CASCADE));
        
    // Slave PIC: enable mouse (IRQ 12) and HDD (IRQ 14)
    outb(PIC_SLAVE_PORT, ~(PIC_MASK_MOUSE | PIC_MASK_HDD));
    
    pmm_init(memory_end_addr, (uint32_t)&kernel_end);
    paging_init(memory_end_addr);
    heap_init();


    // mapping video memory
    uint32_t fb_size = fb_width * fb_height * 4;
    for (uint32_t i = 0; i < fb_size; i += 4096) {
        paging_map(kernel_page_directory, (uint32_t)fb_ptr + i, (uint32_t)fb_ptr + i, 3);
    }

    acpi_init();

    if (cpu_count > 0) {
        uint32_t apic_id_reg = lapic_read(LAPIC_ID);
        int bsp_apic_id = (apic_id_reg >> 24) & 0xFF;

        int bsp_idx = -1;
        for (int i = 0; i < cpu_count; i++) {
            if (cpus[i].id == bsp_apic_id) {
                bsp_idx = i;
                break;
            }
        }

        if (bsp_idx > 0) {
            cpu_t tmp = cpus[0];
            cpus[0] = cpus[bsp_idx];
            cpus[bsp_idx] = tmp;

            cpus[0].index = 0;
            cpus[bsp_idx].index = bsp_idx;
        }
    }

    vga_init();
    vga_init_graphics();

    clipboard_init();
    
    kbd_init();
    mouse_init();
    
    ahci_init();

    yulafs_init();
    yulafs_lookup("/"); 

    kbd_vfs_init();
    console_init();

    proc_init();
    sched_init();

    pc_speaker_init();
    pc_speaker_beep(); 

    for (int i = 0; i < MAX_CPUS; i++) {
        cpus[i].idle_task = proc_create_idle(i);
    }
    
    window_init_system(); 

    proc_spawn_kthread("gui", PRIO_GUI, gui_task, 0);

    smp_boot_aps();
    
    proc_spawn_kthread("reaper", PRIO_HIGH, reaper_task_func, 0);

    ahci_set_async_mode(1);
    proc_spawn_kthread("syncer", PRIO_LOW, syncer_task, 0);

    __asm__ volatile("sti");
    sched_yield();
}