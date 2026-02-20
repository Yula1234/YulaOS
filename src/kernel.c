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
#include <drivers/virtio_gpu.h>
#include <drivers/gpu0.h>
#include <drivers/ne2k.h>

#include <kernel/clipboard.h>
#include <kernel/boot.h>
#include <kernel/init.h>
#include <kernel/tty/tty.h>
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
#include <hal/ioapic.h>
#include <hal/pic.h>

#include <mm/heap.h>
#include <mm/pmm.h>
#include <kernel/panic.h>

#include <kernel/profiler.h>
#include <kernel/symbols.h>

#include <lib/cpp/ctors.h>

#include <stdint.h>

volatile uint32_t kernel_simd_caps;
volatile uint64_t kernel_xsave_mask;

extern uint32_t kernel_end; 

extern void put_pixel(int x, int y, uint32_t color);

extern void smp_boot_aps(void);

#ifndef ENABLE_UHCI
#define ENABLE_UHCI 1
#endif

static int g_enable_uhci = ENABLE_UHCI;

static void kmain_cpu_init(uint32_t magic, multiboot_info_t* mb_info) {
    validate_multiboot(magic, mb_info);

    symbols_init(mb_info);

    kernel_enable_sse();
    cpu_init_system();

    init_fb_info(mb_info);

    gdt_init();
    idt_init();

    kernel_init_simd();

    lapic_init();
    lapic_timer_init(KERNEL_TIMER_HZ);

    if (g_enable_uhci) {
        uhci_quiesce_early();
    }
}

static uint32_t kmain_memory_init(const multiboot_info_t* mb_info) {
    uint32_t memory_end_addr = detect_memory_end(mb_info);
    pic_configure_legacy();

    pmm_init(memory_end_addr, (uint32_t)&kernel_end);
    paging_init(memory_end_addr);
    heap_init();

    return memory_end_addr;
}

static void kmain_video_init(uint32_t memory_end_addr) {
    virtio_gpu_init();
    fb_select_active();
    map_framebuffer(memory_end_addr);

    g_fb_mapped = 1;
}

static void kmain_platform_init(void) {
    acpi_init();
    ensure_bsp_cpu_index_zero();
}

static void kmain_devices_init(void) {
    vga_init();
    vga_init_graphics();

    clipboard_init();

    kbd_init();
    mouse_init();

    init_ioapic_legacy();

    ahci_init();
    ne2k_init();

    kbd_vfs_init();
    console_init();
    mouse_vfs_init();
    fb_vfs_init();
    gpu0_vfs_init();
}

static void kmain_fs_init(void) {
    yulafs_init();
    yulafs_lookup("/");

    pty_init();
}

static void kmain_tasks_init(void) {
    proc_init();
    sched_init();

    pc_speaker_init();
    pc_speaker_beep();

    for (int i = 0; i < MAX_CPUS; i++) {
        cpus[i].idle_task = proc_create_idle(i);
    }
}

static void kmain_handle_kthread_failure(void) {
    vga_set_target(0, 0, 0);
    vga_draw_rect(0, 0, (int)fb_width, (int)fb_height, 0x000000);
    vga_print_at("BOOT ERROR: kthread spawn failed", 16, 16, COLOR_RED);
    vga_mark_dirty(0, 0, (int)fb_width, (int)fb_height);
    vga_flip_dirty();

    while (1) {
        __asm__ volatile("hlt");
    }
}

static void kmain_spawn_core_tasks(void) {
    task_t* tty_t = proc_spawn_kthread("tty", PRIO_GUI, tty_task, 0);
    task_t* init_t = proc_spawn_kthread("init", PRIO_USER, init_task, 0);
    if (!tty_t || !init_t) {
        kmain_handle_kthread_failure();
    }
}

static void kmain_smp_init(void) {
    smp_boot_aps();
 
    if (cpu_count > 1) {
        wait_for_ap_start();
 
        if (ap_running_count > 0 && cpus[1].started) {
            ahci_msi_configure_cpu(1);

            if (ioapic_is_initialized()) {
                ioapic_setup_legacy_routes((uint8_t)cpus[1].id);
            }
        }
    }
}

static void kmain_spawn_service_tasks(void) {
    if (g_enable_uhci) {
        proc_spawn_kthread("uhci", PRIO_LOW, uhci_late_init_task, 0);
    }

    proc_spawn_kthread("reaper", PRIO_HIGH, reaper_task_func, 0);

    ahci_set_async_mode(1);
    proc_spawn_kthread("syncer", PRIO_LOW, syncer_task, 0);

#ifdef KERNEL_PROFILE
    proc_spawn_kthread("profiler", PRIO_LOW, profiler_task, 0);
#endif
}

__attribute__((target("no-sse"))) void kmain(uint32_t magic, multiboot_info_t* mb_info) {
    kmain_cpu_init(magic, mb_info);

    uint32_t memory_end_addr = kmain_memory_init(mb_info);

    cpp_call_global_ctors();

    kmain_video_init(memory_end_addr);
    kmain_platform_init();
    kmain_devices_init();
    kmain_fs_init();
    kmain_tasks_init();
    kmain_spawn_core_tasks();
    kmain_smp_init();
    kmain_spawn_service_tasks();

#ifdef KERNEL_PROFILE
    profiler_init();
#endif

    __asm__ volatile("sti");
    sched_yield();
}
