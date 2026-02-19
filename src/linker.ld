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