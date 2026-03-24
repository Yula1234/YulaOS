// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#include <drivers/serial/serial_core.h>
#include <drivers/virtio/virtio_gpu.h>
#include <drivers/serial/ns16550.h>
#include <drivers/serial/ttyS0.h>
#include <drivers/block/bdev.h>
#include <drivers/pc_speaker.h>
#include <drivers/keyboard.h>
#include <drivers/console.h>
#include <drivers/driver.h>
#include <drivers/mouse.h>
#include <drivers/fbdev.h>
#include <drivers/gpu0.h>
#include <drivers/ne2k.h>
#include <drivers/ahci.h>
#include <drivers/uhci.h>
#include <drivers/acpi.h>
#include <drivers/vga.h>

#include <kernel/symbols/symbols.h>
#include <kernel/output/console.h>
#include <kernel/init/init.h>
#include <kernel/init/boot.h>
#include <kernel/profiler.h>
#include <kernel/smp/cpu.h>
#include <kernel/tty/tty.h>
#include <kernel/sched.h>
#include <kernel/panic.h>
#include <kernel/proc.h>

#include <hal/ioapic.h>
#include <hal/mmio.h>
#include <hal/pmio.h>
#include <hal/apic.h>
#include <hal/simd.h>
#include <hal/pit.h>
#include <hal/pic.h>
#include <hal/io.h>

#include <fs/pty/pty.h>
#include <fs/yulafs.h>
#include <fs/bcache.h>
#include <fs/vfs.h>

#include <arch/i386/paging.h>
#include <arch/i386/gdt.h>
#include <arch/i386/idt.h>

#include <mm/heap.h>
#include <mm/pmm.h>
#include <mm/vmm.h>

#include <lib/cpp/ctors.h>
#include <lib/string.h>

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

typedef struct {
    uint32_t mod_start;
    uint32_t mod_end;
    uint32_t string;
    uint32_t reserved;
} multiboot_module_t;

static uint32_t clamp_u64_to_u32(uint64_t value) {
    if (value > 0xFFFFFFFFull) {
        return 0xFFFFFFFFu;
    }

    return (uint32_t)value;
}

static void add_region(
    pmm_region_t* regions,
    uint32_t* count,
    uint32_t max_count,
    uint32_t base,
    uint32_t size,
    uint32_t type
) {
    if (!regions || !count || size == 0u) {
        return;
    }

    if (*count >= max_count) {
        return;
    }

    regions[*count].base = base;
    regions[*count].size = size;
    regions[*count].type = type;
    (*count)++;
}

static void add_reserved(
    pmm_reserved_region_t* reserved,
    uint32_t* count,
    uint32_t max_count,
    uint32_t base,
    uint32_t size
) {
    if (!reserved || !count || size == 0u) {
        return;
    }

    if (*count >= max_count) {
        return;
    }

    reserved[*count].base = base;
    reserved[*count].size = size;
    (*count)++;
}

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

    const uint32_t kernel_end_addr = (uint32_t)&kernel_end;

    const uint32_t max_regions = 128u;
    const uint32_t max_reserved = 128u;

    pmm_region_t regions[max_regions];
    pmm_reserved_region_t reserved[max_reserved];

    uint32_t region_count = 0u;
    uint32_t reserved_count = 0u;

    if (mb_info && (mb_info->flags & (1u << 6))) {
        uint32_t mmap_base = mb_info->mmap_addr;
        uint32_t mmap_len = mb_info->mmap_length;

        uint32_t off = 0u;
        while (off + sizeof(uint32_t) <= mmap_len) {
            multiboot_memory_map_t* e = (multiboot_memory_map_t*)(mmap_base + off);
            if (e->size == 0u) {
                break;
            }

            uint64_t addr64 = e->addr;
            uint64_t len64 = e->len;
            if (len64 != 0u) {
                uint32_t base = clamp_u64_to_u32(addr64);
                uint32_t end = clamp_u64_to_u32(addr64 + len64);
                if (end > base) {
                    uint32_t type = e->type == 1u ? PMM_REGION_AVAILABLE : PMM_REGION_RESERVED;
                    add_region(regions, &region_count, max_regions, base, end - base, type);
                }
            }

            uint32_t step = e->size + sizeof(uint32_t);
            if (step > mmap_len - off) {
                break;
            }
            off += step;
        }
    } else {
        add_region(regions, &region_count, max_regions, 0u, memory_end_addr, PMM_REGION_AVAILABLE);
    }

    add_reserved(reserved, &reserved_count, max_reserved, 0u, kernel_end_addr);

    if (mb_info) {
        add_reserved(
            reserved,
            &reserved_count,
            max_reserved,
            (uint32_t)mb_info,
            (uint32_t)sizeof(multiboot_info_t)
        );

        if (mb_info->flags & (1u << 6)) {
            add_reserved(reserved, &reserved_count, max_reserved, mb_info->mmap_addr, mb_info->mmap_length);
        }

        if (mb_info->flags & (1u << 11)) {
            if (mb_info->elf_num != 0u && mb_info->elf_size != 0u && mb_info->elf_addr != 0u) {
                uint64_t bytes64 = (uint64_t)mb_info->elf_num * (uint64_t)mb_info->elf_size;
                uint32_t bytes = clamp_u64_to_u32(bytes64);
                add_reserved(reserved, &reserved_count, max_reserved, mb_info->elf_addr, bytes);
            }
        }

        if (mb_info->flags & (1u << 3)) {
            if (mb_info->mods_count != 0u && mb_info->mods_addr != 0u) {
                uint32_t mods_bytes = mb_info->mods_count * (uint32_t)sizeof(multiboot_module_t);
                add_reserved(reserved, &reserved_count, max_reserved, mb_info->mods_addr, mods_bytes);

                multiboot_module_t* mods = (multiboot_module_t*)mb_info->mods_addr;
                for (uint32_t i = 0u; i < mb_info->mods_count; i++) {
                    uint32_t start = mods[i].mod_start;
                    uint32_t end = mods[i].mod_end;
                    if (end > start) {
                        add_reserved(reserved, &reserved_count, max_reserved, start, end - start);
                    }
                }
            }
        }

        if (mb_info->flags & (1u << 12)) {
            uint64_t fb_addr64 = mb_info->framebuffer_addr;
            uint64_t fb_size64 = (uint64_t)mb_info->framebuffer_pitch * (uint64_t)mb_info->framebuffer_height;
            if (fb_addr64 <= 0xFFFFFFFFull && fb_size64 != 0u && fb_size64 <= 0xFFFFFFFFull) {
                uint32_t fb_base = (uint32_t)fb_addr64;
                uint32_t fb_size = (uint32_t)fb_size64;
                add_reserved(reserved, &reserved_count, max_reserved, fb_base, fb_size);
            }
        }
    }

    pmm_init_regions(regions, region_count, reserved, reserved_count, kernel_end_addr);
    paging_init(memory_end_addr);
    vmm_init();
    heap_init();

    pmio_init();
    mmio_init();

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

    ns16550_init(NS16550_COM1);

    serial_core_init(NS16550_COM1);
    console_set_writer(serial_core_console_write, 0);

    kbd_init();
    mouse_init();

    init_ioapic_legacy();

    bdev_init();

    ahci_init();
    ne2k_init();

    drivers_init_stage(DRIVER_STAGE_CORE);

}

static void kmain_fs_init(void) {
    block_device_t* root_bdev = bdev_find_by_name("sd0");

    if (!root_bdev) {
        root_bdev = bdev_first();
    }

    bdev_set_root(root_bdev);
    bcache_attach_device(root_bdev);

    yulafs_init();
    vfs_init();

    ttyS0_init();
    mouse_vfs_init();
    fb_vfs_init();
    gpu0_vfs_init();

    drivers_init_stage(DRIVER_STAGE_VFS);

    yulafs_lookup("/");
    pty_init();
}

static void kmain_tasks_init(void) {
    proc_init();
    sched_init();

    pc_speaker_init();
    pc_speaker_beep();

    for (int i = 0; i < cpu_count; i++) {
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

    proc_spawn_kthread("rcu", PRIO_LOW, rcu_gc_task, 0);

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
    kmain_tasks_init();

    kmain_devices_init();
    kmain_fs_init();
    
    kmain_spawn_core_tasks();
    kmain_smp_init();
    kmain_spawn_service_tasks();

#ifdef KERNEL_PROFILE
    profiler_init();
#endif

    __asm__ volatile("sti");
    sched_yield();
}
