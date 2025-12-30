; SPDX-License-Identifier: GPL-2.0
; Copyright (C) 2025 Yula1234

format ELF

public start
extrn kmain

section '.multiboot' align 4
    MULTIBOOT_PAGE_ALIGN  equ 1 shl 0
    MULTIBOOT_MEMORY_INFO equ 1 shl 1
    MULTIBOOT_VIDEO_MODE  equ 1 shl 2

    FLAGS    equ MULTIBOOT_PAGE_ALIGN or MULTIBOOT_MEMORY_INFO or MULTIBOOT_VIDEO_MODE
    MAGIC    equ 0x1BADB002
    CHECKSUM equ -(MAGIC + FLAGS)

    dd MAGIC
    dd FLAGS
    dd CHECKSUM
    
    dd 0 ; header_addr
    dd 0 ; load_addr
    dd 0 ; load_end_addr
    dd 0 ; bss_end_addr
    dd 0 ; entry_addr
    
    dd 0    ; mode_type (0 = linear framebuffer)
    dd 1024 ; width
    dd 768  ; height
    dd 32   ; depth

section '.text' executable align 4

start:
    cli
    mov esp, stack_top
    
    push ebx 
    push eax
    
    call kmain

.hang:
    hlt
    jmp .hang

section '.bss' writeable align 16
stack_bottom:
    rb 65536
stack_top:

section '.note.GNU-stack'