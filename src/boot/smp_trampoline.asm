format binary
use16
org 0

    db 0xEB, 0x12 
    db 0x90, 0x90 
    
    stack_ptr: dd 0
    code_ptr: dd 0
    cr3_ptr: dd 0
    arg_ptr: dd 0

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
    dq 0x00CF9A000000FFFF 
    dq 0x00CF92000000FFFF 
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

    mov ebx, 0x1000
    
    ; CR3
    mov eax, [ebx + 12] 
    mov cr3, eax
    
    mov eax, cr0
    or eax, 0x80000000
    mov cr0, eax

    ; Stack
    mov esp, [ebx + 4]

    ; Argument
    mov eax, [ebx + 16]
    push eax 
    
    push 0
    mov eax, [ebx + 8]
    jmp eax

.hang:
    cli
    hlt
    jmp .hang