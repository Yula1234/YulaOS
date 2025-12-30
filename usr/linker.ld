/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2025 Yula1234 */


ENTRY(_start)

SECTIONS
{
    . = 0x08048000;

    .text : ALIGN(32)
    {
        *(.text)
        *(.text.*)
    }

    .rodata : ALIGN(32)
    {
        *(.rodata)
        *(.rodata.*)
    }

    .data : ALIGN(32)
    {
        *(.data)
        *(.data.*)
    }

    .bss : ALIGN(32)
    {
        *(COMMON)
        *(.bss)
    }
}