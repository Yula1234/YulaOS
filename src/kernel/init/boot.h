// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 Yula1234

#ifndef KERNEL_BOOT_H
#define KERNEL_BOOT_H

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

    uint32_t elf_num;
    uint32_t elf_size;
    uint32_t elf_addr;
    uint32_t elf_shndx;

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

void validate_multiboot(uint32_t magic, const multiboot_info_t* mb_info);
void init_fb_info(const multiboot_info_t* mb_info);
uint32_t detect_memory_end(const multiboot_info_t* mb_info);
void map_framebuffer(uint32_t memory_end_addr);
void ensure_bsp_cpu_index_zero(void);
void init_ioapic_legacy(void);
void ioapic_setup_legacy_routes(uint8_t cpu_apic_id);
void wait_for_ap_start(void);
void fb_select_active(void);

#endif
