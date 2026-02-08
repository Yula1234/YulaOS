// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 Yula1234

#include <drivers/acpi.h>
#include <drivers/virtio_gpu.h>

#include <hal/apic.h>
#include <hal/io.h>
#include <hal/ioapic.h>
#include <hal/irq.h>
#include <hal/pit.h>
#include <hal/pic.h>

#include <arch/i386/paging.h>

#include <kernel/cpu.h>

#include "boot.h"

extern uint32_t* fb_ptr;
extern uint32_t  fb_width;
extern uint32_t  fb_height;
extern uint32_t  fb_pitch;

static void halt_forever(void) {
    __asm__ volatile("cli");
    while (1) __asm__ volatile("hlt");
}

void validate_multiboot(uint32_t magic, const multiboot_info_t* mb_info) {
    if (magic != 0x2BADB002) {
        halt_forever();
    }

    if (!(mb_info->flags & (1u << 12))) {
        halt_forever();
    }

    if (mb_info->framebuffer_type != 1 || mb_info->framebuffer_bpp != 32) {
        halt_forever();
    }

    if (mb_info->framebuffer_width == 0 || mb_info->framebuffer_height == 0 ||
        mb_info->framebuffer_pitch < mb_info->framebuffer_width * 4) {
        halt_forever();
    }
}

void init_fb_info(const multiboot_info_t* mb_info) {
    fb_ptr = (uint32_t*)(uint32_t)mb_info->framebuffer_addr;
    fb_width = mb_info->framebuffer_width;
    fb_height = mb_info->framebuffer_height;
    fb_pitch = mb_info->framebuffer_pitch;
}

uint32_t detect_memory_end(const multiboot_info_t* mb_info) {
    uint64_t memory_end_addr64 = 0;

    if (mb_info->flags & (1u << 6)) {
        uint32_t mmap_base = mb_info->mmap_addr;
        uint32_t mmap_len = mb_info->mmap_length;

        uint32_t off = 0;
        while (off + sizeof(uint32_t) <= mmap_len) {
            multiboot_memory_map_t* e = (multiboot_memory_map_t*)(mmap_base + off);
            if (e->size == 0) {
                break;
            }

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
    } else if (mb_info->flags & (1u << 0)) {
        memory_end_addr64 = (uint64_t)(mb_info->mem_upper * 1024) + 0x100000ull;
    }

    if (memory_end_addr64 == 0) {
        memory_end_addr64 = 1024ull * 1024ull * 64ull;
    }

    return (uint32_t)memory_end_addr64;
}

void map_framebuffer(uint32_t memory_end_addr) {
    uint64_t fb_size64 = (uint64_t)fb_pitch * (uint64_t)fb_height;
    if (fb_size64 == 0 || fb_size64 > 0xFFFFFFFFu) {
        return;
    }

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
        for (uint32_t i = 0; i < fb_map_size; i += 4096u) {
            paging_map(kernel_page_directory, fb_page + i, fb_page + i, fb_flags);
        }
    }
}

void ensure_bsp_cpu_index_zero(void) {
    if (cpu_count <= 0) {
        return;
    }

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

void ioapic_setup_legacy_routes(uint8_t cpu_apic_id) {
    uint32_t gsi;
    int active_low;
    int level_trigger;

    if (!acpi_get_iso(1, &gsi, &active_low, &level_trigger)) {
        gsi = 1;
        active_low = 0;
        level_trigger = 0;
    }
    ioapic_route_gsi(gsi, (uint8_t)(32 + 1), cpu_apic_id, active_low, level_trigger);

    if (!acpi_get_iso(12, &gsi, &active_low, &level_trigger)) {
        gsi = 12;
        active_low = 0;
        level_trigger = 0;
    }
    ioapic_route_gsi(gsi, (uint8_t)(32 + 12), cpu_apic_id, active_low, level_trigger);
}

void init_ioapic_legacy(void) {
    if (ioapic_is_initialized() != 0) {
        return;
    }

    uint32_t ioapic_phys = 0;
    uint32_t ioapic_gsi_base = 0;
    if (!acpi_get_ioapic(&ioapic_phys, &ioapic_gsi_base)) {
        return;
    }

    if (!ioapic_init(ioapic_phys, ioapic_gsi_base)) {
        return;
    }

    ioapic_setup_legacy_routes((uint8_t)cpus[0].id);

    outb(0x22, 0x70);
    io_wait();
    outb(0x23, 0x01);
    io_wait();

    irq_set_legacy_pic_enabled(0);
    outb(0x21, 0xFF);
    outb(0xA1, 0xFF);
}

void wait_for_ap_start(void) {
    for (volatile int i = 0; i < 2000000; i++) {
        if (ap_running_count > 0 && cpus[1].started) break;
        __asm__ volatile("pause");
    }
}

void fb_select_active(void) {
    if (!virtio_gpu_is_active()) return;
    const virtio_gpu_fb_t* fb = virtio_gpu_get_fb();
    if (!fb || !fb->fb_ptr || fb->width == 0 || fb->height == 0 || fb->pitch == 0) return;
    fb_ptr = fb->fb_ptr;
    fb_width = fb->width;
    fb_height = fb->height;
    fb_pitch = fb->pitch;
}
