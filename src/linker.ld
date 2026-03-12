/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2025 Yula1234 */

ENTRY(start)

SECTIONS
{
    . = 0x100000;

    .text : ALIGN(0x1000)
    {
        KEEP(*(.multiboot))
        *(.text)
        *(.text*)
    }

    .rodata : ALIGN(0x1000)
    {
        *(.rodata)
        *(.rodata*)
    }

    .uaccess_fixup : ALIGN(4)
    {
        __uaccess_fixup_start = .;
        KEEP(*(.uaccess_fixup))
        __uaccess_fixup_end = .;
    }

    .yosdrivers : ALIGN(8)
    {
        __yosdrivers_start = .;
        KEEP(*(.yosdrivers*))
        __yosdrivers_end = .;
    }

    .init_array : ALIGN(0x1000)
    {
        __init_array_start = .;
        KEEP(*(.init_array*))
        __init_array_end = .;
    }

    .data : ALIGN(0x1000)
    {
        *(.data)
        *(.data*)
    }

    .bss : ALIGN(0x1000)
    {
        *(COMMON)
        *(.bss)
        *(.bss*)
    }

    . = ALIGN(4096);
    kernel_end = .;
}