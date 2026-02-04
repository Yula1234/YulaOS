// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#include <lib/string.h>

#include <fs/yulafs.h>
#include <fs/bcache.h>
#include <fs/pty.h>
#include <fs/vfs.h>

#include <drivers/pc_speaker.h>
#include <drivers/keyboard.h>
#include <drivers/console.h>
#include <drivers/mouse.h>
#include <drivers/fbdev.h>
#include <drivers/ahci.h>
#include <drivers/uhci.h>
#include <drivers/acpi.h> 
#include <drivers/vga.h>
#include <drivers/pci.h>
#include <drivers/virtio_gpu.h>
 #include <drivers/gpu0.h>

#include <kernel/clipboard.h>
#include <kernel/tty.h>
#include <kernel/sched.h>
#include <kernel/proc.h>
#include <kernel/cpu.h>
#include <kernel/input_focus.h>

#include <arch/i386/paging.h>
#include <arch/i386/gdt.h>
#include <arch/i386/idt.h>

#include <hal/apic.h>
#include <hal/irq.h>
#include <hal/lock.h>
#include <hal/simd.h>
#include <hal/pit.h>
#include <hal/io.h>
#include <hal/ioapic.h>

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

    union {
        struct {
            uint32_t framebuffer_palette_addr;
            uint16_t framebuffer_palette_num_colors;
        } __attribute__((packed)) palette;

        struct {
            uint8_t framebuffer_red_field_position;
            uint8_t framebuffer_red_mask_size;
            uint8_t framebuffer_green_field_position;
            uint8_t framebuffer_green_mask_size;
            uint8_t framebuffer_blue_field_position;
            uint8_t framebuffer_blue_mask_size;
        } __attribute__((packed)) rgb;
    } framebuffer_color_info;
} __attribute__((packed)) multiboot_info_t;

volatile uint32_t kernel_simd_caps;
volatile uint64_t kernel_xsave_mask;

uint32_t* fb_ptr;
uint32_t  fb_width;
uint32_t  fb_height;
uint32_t  fb_pitch;

extern uint32_t kernel_end; 

extern void put_pixel(int x, int y, uint32_t color);

extern void smp_boot_aps(void);

#ifndef ENABLE_UHCI
#define ENABLE_UHCI 1
#endif

static int g_enable_uhci = ENABLE_UHCI;

volatile int g_fb_mapped = 0;

static inline uint16_t pci_read16_k(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint32_t reg = pci_read(bus, slot, func, offset & 0xFCu);
    return (uint16_t)((reg >> ((offset & 2u) * 8u)) & 0xFFFFu);
}

static inline void pci_write16_k(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint16_t value) {
    uint8_t aligned = offset & 0xFCu;
    uint32_t reg = pci_read(bus, slot, func, aligned);
    uint32_t shift = (uint32_t)(offset & 2u) * 8u;
    reg &= ~(0xFFFFu << shift);
    reg |= ((uint32_t)value << shift);
    pci_write(bus, slot, func, aligned, reg);
}

static void usb_quiesce_early(void) {
    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t slot = 0; slot < 32; slot++) {
            for (uint8_t func = 0; func < 8; func++) {
                uint16_t vendor = (uint16_t)(pci_read((uint8_t)bus, slot, func, 0x00u) & 0xFFFFu);
                if (vendor == 0xFFFFu) continue;

                uint32_t reg = pci_read((uint8_t)bus, slot, func, 0x08u);
                uint8_t class_code = (uint8_t)((reg >> 24) & 0xFFu);
                uint8_t subclass = (uint8_t)((reg >> 16) & 0xFFu);
                uint8_t prog_if = (uint8_t)((reg >> 8) & 0xFFu);

                if (class_code != 0x0Cu || subclass != 0x03u) {
                    continue;
                }

                if (prog_if == 0x00u) {
                    uint32_t bar4 = pci_read((uint8_t)bus, slot, func, 0x20u);
                    uint16_t io_base = (uint16_t)(bar4 & 0xFFFCu);
                    if (io_base) {
                        uint16_t cmd = pci_read16_k((uint8_t)bus, slot, func, 0x04u);
                        if ((cmd & 0x0001u) == 0u) {
                            pci_write16_k((uint8_t)bus, slot, func, 0x04u, (uint16_t)(cmd | 0x0001u));
                        }

                        outw((uint16_t)(io_base + 0x00u), 0);
                        outw((uint16_t)(io_base + 0x00u), (uint16_t)(1u << 1));
                        for (uint32_t i = 0; i < 100000u; i++) {
                            if ((inw((uint16_t)(io_base + 0x00u)) & (1u << 1)) == 0u) break;
                        }
                        outw((uint16_t)(io_base + 0x02u), 0xFFFFu);
                    }
                }

                uint16_t cmd = pci_read16_k((uint8_t)bus, slot, func, 0x04u);
                cmd &= (uint16_t)~(0x0001u);
                cmd &= (uint16_t)~(0x0002u);
                cmd &= (uint16_t)~(0x0004u);
                cmd |= (uint16_t)(1u << 10);
                pci_write16_k((uint8_t)bus, slot, func, 0x04u, cmd);
            }
        }
    }
}

static inline void sys_usleep(uint32_t us) {
    __asm__ volatile("int $0x80" : : "a"(11), "b"(us));
}

static void fb_select_active(void) {
    if (!virtio_gpu_is_active()) return;
    const virtio_gpu_fb_t* fb = virtio_gpu_get_fb();
    if (!fb || !fb->fb_ptr || fb->width == 0 || fb->height == 0 || fb->pitch == 0) return;
    fb_ptr = fb->fb_ptr;
    fb_width = fb->width;
    fb_height = fb->height;
    fb_pitch = fb->pitch;
}

static void init_task(void* arg) {
    (void)arg;

    if (yulafs_lookup("/bin") == -1) (void)yulafs_mkdir("/bin");
    if (yulafs_lookup("/home") == -1) (void)yulafs_mkdir("/home");

    task_t* self = proc_current();
    if (!self) return;

    term_instance_t* term = (term_instance_t*)kmalloc(sizeof(*term));
    if (!term) return;
    memset(term, 0, sizeof(*term));

    int cols = (int)(fb_width / 8);
    int view_rows = (int)(fb_height / 16);
    if (cols < 1) cols = 1;
    if (view_rows < 1) view_rows = 1;
    term->cols = cols;
    term->view_rows = view_rows;
    term->curr_fg = 0xD4D4D4;
    term->curr_bg = 0x141414;
    term_init(term);

    self->terminal = term;
    self->term_mode = 1;
    tty_set_terminal(term);

    (void)vfs_open("/dev/kbd", 0);
    (void)vfs_open("/dev/console", 0);
    (void)vfs_open("/dev/console", 0);

    spinlock_acquire(&term->lock);
    term_putc(term, 0x0C);
    spinlock_release(&term->lock);

    uint32_t home_inode = (uint32_t)yulafs_lookup("/home");
    if ((int)home_inode == -1) home_inode = 1;
    self->cwd_inode = home_inode;

    for (;;) {
        char* argv[] = { "ush", 0 };
        task_t* child = proc_spawn_elf("/bin/ush.exe", 1, argv);
        if (!child) {
            spinlock_acquire(&term->lock);
            term_print(term, "init: failed to spawn /bin/ush.exe\n");
            spinlock_release(&term->lock);
            sys_usleep(200000);
            continue;
        }

        input_focus_set_pid(child->pid);
        proc_wait(child->pid);
        input_focus_set_pid(self->pid);

        spinlock_acquire(&term->lock);
        term_print(term, "[ush exited]\n");
        spinlock_release(&term->lock);
        sys_usleep(200000);
    }
}

static void uhci_late_init_task(void* arg) {
    (void)arg;
    uhci_init();
    uhci_late_init();

    while (1) {
        uhci_poll();
        sys_usleep(2000);
    }
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
        __asm__ volatile("cli");
        while (1) __asm__ volatile("hlt");
    }

    if (!(mb_info->flags & (1u << 12))) {
        __asm__ volatile("cli");
        while (1) __asm__ volatile("hlt");
    }

    if (mb_info->framebuffer_type != 1 || mb_info->framebuffer_bpp != 32) {
        __asm__ volatile("cli");
        while (1) __asm__ volatile("hlt");
    }

    if (mb_info->framebuffer_width == 0 || mb_info->framebuffer_height == 0 ||
        mb_info->framebuffer_pitch < mb_info->framebuffer_width * 4) {
        __asm__ volatile("cli");
        while (1) __asm__ volatile("hlt");
    }

    kernel_enable_sse();
    cpu_init_system();

    fb_ptr = (uint32_t*)(uint32_t)mb_info->framebuffer_addr;
    fb_width = mb_info->framebuffer_width;
    fb_height = mb_info->framebuffer_height;
    fb_pitch = mb_info->framebuffer_pitch;

    gdt_init();
    idt_init();

    kernel_init_simd();

    lapic_init();
    lapic_timer_init(15000);

    if (g_enable_uhci) {
        usb_quiesce_early();
    }

    uint64_t memory_end_addr64 = 0;

    if (mb_info->flags & (1 << 6)) {
        uint32_t mmap_base = mb_info->mmap_addr;
        uint32_t mmap_len = mb_info->mmap_length;

        uint32_t off = 0;
        uint32_t entries_printed = 0;
        while (off + sizeof(uint32_t) <= mmap_len) {
            multiboot_memory_map_t* e = (multiboot_memory_map_t*)(mmap_base + off);
            if (e->size == 0) {
                break;
            }

            (void)entries_printed;

            if (e->type == 1) {
                uint64_t end = e->addr + e->len;
                if (end > 0xFFFFFFFFull) end = 0xFFFFFFFFull;
                if (end > memory_end_addr64) {
                    memory_end_addr64 = end;
                }
            }

            uint32_t step = e->size + sizeof(uint32_t);
            if (step > mmap_len - off) {
                break;
            }
            off += step;
        }
    }

    else if (mb_info->flags & (1 << 0)) {
        memory_end_addr64 = (uint64_t)(mb_info->mem_upper * 1024) + 0x100000ull;
    }

    if (memory_end_addr64 == 0) {
        memory_end_addr64 = 1024ull * 1024ull * 64ull;
    }

    uint32_t memory_end_addr = (uint32_t)memory_end_addr64;
    
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

    virtio_gpu_init();
    fb_select_active();

    // mapping video memory
    uint64_t fb_size64 = (uint64_t)fb_pitch * (uint64_t)fb_height;
    if (fb_size64 != 0 && fb_size64 <= 0xFFFFFFFFu) {
        uint32_t fb_size = (uint32_t)fb_size64;
        uint32_t fb_flags = PTE_PRESENT | PTE_RW;
        if (paging_pat_is_supported()) {
            fb_flags |= PTE_PAT;
        }

        uint32_t fb_base = (uint32_t)fb_ptr;
        if (fb_base >= memory_end_addr) {
            paging_init_mtrr_wc(fb_base, fb_size);
        }

        uint32_t fb_page = fb_base & ~0xFFFu;
        uint32_t fb_end = fb_base + fb_size;
        if (fb_end >= fb_base) {
            uint32_t fb_map_size = (fb_end - fb_page + 0xFFFu) & ~0xFFFu;
            for (uint32_t i = 0; i < fb_map_size; i += 4096) {
                paging_map(kernel_page_directory, fb_page + i, fb_page + i, fb_flags);
            }
        }
    }

    g_fb_mapped = 1;

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

    if (ioapic_is_initialized() == 0) {
        uint32_t ioapic_phys = 0;
        uint32_t ioapic_gsi_base = 0;
        if (acpi_get_ioapic(&ioapic_phys, &ioapic_gsi_base)) {
            if (ioapic_init(ioapic_phys, ioapic_gsi_base)) {
                uint32_t gsi;
                int active_low;
                int level_trigger;

                if (!acpi_get_iso(1, &gsi, &active_low, &level_trigger)) {
                    gsi = 1;
                    active_low = 0;
                    level_trigger = 0;
                }
                ioapic_route_gsi(gsi, (uint8_t)(32 + 1), (uint8_t)cpus[0].id, active_low, level_trigger);

                if (!acpi_get_iso(12, &gsi, &active_low, &level_trigger)) {
                    gsi = 12;
                    active_low = 0;
                    level_trigger = 0;
                }
                ioapic_route_gsi(gsi, (uint8_t)(32 + 12), (uint8_t)cpus[0].id, active_low, level_trigger);
                outb(0x22, 0x70);
                io_wait();
                outb(0x23, 0x01);
                io_wait();

                irq_set_legacy_pic_enabled(0);
                outb(0x21, 0xFF);
                outb(0xA1, 0xFF);
            }
        }
    }

    ahci_init();

    yulafs_init();
    yulafs_lookup("/"); 
 
    kbd_vfs_init();
    console_init();
    mouse_vfs_init();
    fb_vfs_init();
    gpu0_vfs_init();

    pty_init();
 
    proc_init();
    sched_init();

    pc_speaker_init();
    pc_speaker_beep(); 

    for (int i = 0; i < MAX_CPUS; i++) {
        cpus[i].idle_task = proc_create_idle(i);
    }
     
    task_t* tty_t = proc_spawn_kthread("tty", PRIO_GUI, tty_task, 0);
    task_t* init_t = proc_spawn_kthread("init", PRIO_USER, init_task, 0);
    if (!tty_t || !init_t) {
        vga_set_target(0, 0, 0);
        vga_draw_rect(0, 0, (int)fb_width, (int)fb_height, 0x000000);
        vga_print_at("BOOT ERROR: kthread spawn failed", 16, 16, COLOR_RED);
        vga_mark_dirty(0, 0, (int)fb_width, (int)fb_height);
        vga_flip_dirty();
        for (;;) __asm__ volatile("hlt");
    }

    smp_boot_aps();
 
    if (cpu_count > 1) {
        for (volatile int i = 0; i < 2000000; i++) {
            if (ap_running_count > 0 && cpus[1].started) break;
            __asm__ volatile("pause");
        }
 
        if (ap_running_count > 0 && cpus[1].started) {
            ahci_msi_configure_cpu(1);

            if (ioapic_is_initialized()) {
                uint32_t gsi;
                int active_low;
                int level_trigger;

                if (!acpi_get_iso(1, &gsi, &active_low, &level_trigger)) {
                    gsi = 1;
                    active_low = 0;
                    level_trigger = 0;
                }
                ioapic_route_gsi(gsi, (uint8_t)(32 + 1), (uint8_t)cpus[1].id, active_low, level_trigger);

                if (!acpi_get_iso(12, &gsi, &active_low, &level_trigger)) {
                    gsi = 12;
                    active_low = 0;
                    level_trigger = 0;
                }
                ioapic_route_gsi(gsi, (uint8_t)(32 + 12), (uint8_t)cpus[1].id, active_low, level_trigger);
            }
        }
    }

    if (g_enable_uhci) {
        proc_spawn_kthread("uhci", PRIO_LOW, uhci_late_init_task, 0);
    }

    proc_spawn_kthread("reaper", PRIO_HIGH, reaper_task_func, 0);

    ahci_set_async_mode(1);
    proc_spawn_kthread("syncer", PRIO_LOW, syncer_task, 0);

    __asm__ volatile("sti");
    sched_yield();
}
