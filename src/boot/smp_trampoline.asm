format binary
use16
org 0
    jmp start_16
    nop
    nop
    dd 0 
    dd 0 
    dd 0

start_16:
    cli
    
    mov ax, cs
    mov ds, ax

    lgdt [gdt_ptr]

    mov eax, cr0
    or eax, 1
    mov cr0, eax

    db 0x66
    db 0xEA
    dd start_32 + 0x1000
    dw 0x08

align 4
gdt_start:
    dq 0x0000000000000000
    dq 0x00CF9A000000FFFF ; Code
    dq 0x00CF92000000FFFF ; Data
gdt_end:

gdt_ptr:
    dw gdt_end - gdt_start - 1
    dd gdt_start + 0x1000

use32
start_32:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    
    mov eax, [0x100C]
    mov cr3, eax

    mov eax, cr0
    or eax, 0x80000000
    mov cr0, eax
    mov esp, [0x1004]

    mov eax, [0x1008]
    call eax

.hang:
    cli
    hlt
    jmp .hang