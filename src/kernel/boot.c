// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 Yula1234

#include <drivers/acpi.h>
#include <drivers/fbdev.h>
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

static inline uint32_t align_up_4k_u32(uint32_t v) {
    return (v + 0xFFFu) & ~0xFFFu;
}

static inline uint32_t max_u32(uint32_t a, uint32_t b) {
    return (a > b) ? a : b;
}

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
    fb_phys = (uint32_t)mb_info->framebuffer_addr;
    fb_ptr = (uint32_t*)(uintptr_t)(FB_VIRT_BASE + (fb_phys & 0xFFFu));
    fb_width = mb_info->framebuffer_width;
    fb_height = mb_info->framebuffer_height;
    fb_pitch = mb_info->framebuffer_pitch;
}

uint32_t detect_memory_end(const multiboot_info_t* mb_info) {
    uint64_t memory_end_addr64 = 0;

    const uint64_t low_4g_excl = 0x100000000ull;

    if (mb_info->flags & (1u << 6)) {
        uint32_t mmap_base = mb_info->mmap_addr;
        uint32_t mmap_len = mb_info->mmap_length;

        uint32_t off = 0;
        while (off + sizeof(uint32_t) <= mmap_len) {
            multiboot_memory_map_t* e = (multiboot_memory_map_t*)(mmap_base + off);
            if (e->size == 0) {
                break;
            }

            const uint32_t step = e->size + sizeof(uint32_t);
            if (step > mmap_len - off) {
                break;
            }

            if (e->type == 1) {
                const uint64_t start = e->addr;
                uint64_t end = start + e->len;
                if (end < start) {
                    end = low_4g_excl;
                }

                if (start < low_4g_excl) {
                    if (end > low_4g_excl) {
                        end = low_4g_excl;
                    }

                    if (end > memory_end_addr64) {
                        memory_end_addr64 = end;
                    }
                }
            }

            off += step;
        }
    } else if (mb_info->flags & (1u << 0)) {
        memory_end_addr64 = (uint64_t)(mb_info->mem_upper * 1024) + 0x100000ull;
    }

    if (memory_end_addr64 == 0) {
        memory_end_addr64 = 1024ull * 1024ull * 64ull;
    }

    if (memory_end_addr64 > 0xFFFFFFFFull) {
        memory_end_addr64 = 0xFFFFFFFFull;
    }

    return (uint32_t)memory_end_addr64;
}

uint32_t multiboot_identity_map_end(const multiboot_info_t* mb_info) {
    if (!mb_info) {
        return 0;
    }

    uint32_t end = 0;

    {
        uint32_t mb = (uint32_t)(uintptr_t)mb_info;
        end = max_u32(end, mb + (uint32_t)sizeof(*mb_info));
    }

    if ((mb_info->flags & (1u << 6)) != 0u) {
        uint32_t mmap_base = mb_info->mmap_addr;
        uint32_t mmap_len = mb_info->mmap_length;
        if (mmap_base != 0 && mmap_len != 0 && mmap_base + mmap_len >= mmap_base) {
            end = max_u32(end, mmap_base + mmap_len);
        }
    }

    if ((mb_info->flags & (1u << 11)) != 0u) {
        uint32_t elf_base = mb_info->elf_addr;
        uint32_t elf_num = mb_info->elf_num;
        uint32_t elf_entsz = mb_info->elf_size;

        if (elf_base != 0 && elf_num != 0 && elf_entsz != 0) {
            uint32_t table_size = 0;
            if (elf_num <= 0xFFFFFFFFu / elf_entsz) {
                table_size = elf_num * elf_entsz;
            }

            if (table_size != 0 && elf_base + table_size >= elf_base) {
                end = max_u32(end, elf_base + table_size);
            }
        }
    }

    return align_up_4k_u32(end);
}

void map_framebuffer(uint32_t memory_end_addr) {
    uint64_t fb_size64 = (uint64_t)fb_pitch * (uint64_t)fb_height;
    if (fb_size64 == 0 || fb_size64 > 0xFFFFFFFFu) {
        return;
    }

    uint32_t fb_size = (uint32_t)fb_size64;
    uint32_t fb_flags = PTE_PRESENT | PTE_RW | PTE_PCD | PTE_PWT;

    (void)memory_end_addr;
    (void)fb_size;

    const uint32_t phys_page = fb_phys & ~0xFFFu;

    const uint32_t existing_phys = paging_get_phys(kernel_page_directory, FB_VIRT_BASE);
    if ((existing_phys & ~0xFFFu) == phys_page) {
        return;
    }
    const uint32_t virt_page = FB_VIRT_BASE;

    const uint32_t fb_end = fb_phys + fb_size;
    if (fb_end >= fb_phys) {
        const uint32_t phys_end_page = (fb_end + 0xFFFu) & ~0xFFFu;
        const uint32_t map_size = phys_end_page - phys_page;

        for (uint32_t off = 0; off < map_size; off += 4096u) {
            paging_map(kernel_page_directory, virt_page + off, phys_page + off, fb_flags);
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

    fb_phys = fb->fb_phys;
    fb_ptr = (uint32_t*)(uintptr_t)(FB_VIRT_BASE + (fb_phys & 0xFFFu));
    fb_width = fb->width;
    fb_height = fb->height;
    fb_pitch = fb->pitch;
}
