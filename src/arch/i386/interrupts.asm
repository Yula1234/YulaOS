; SPDX-License-Identifier: GPL-2.0
; Copyright (C) 2025 Yula1234

format ELF
section '.text' executable

public isr_stub_table
public load_page_directory
public enable_paging
extrn isr_handler

load_page_directory:
    mov eax, [esp + 4]
    mov cr3, eax
    ret

enable_paging:
    mov eax, cr0
    or eax, 0x80000000
    mov cr0, eax
    ret

df_putc:
    push eax
    push ebx
    push edx

    mov bl, al

    mov dx, 0xE9
    mov al, bl
    out dx, al

    mov dx, 0x3FD
df_uart_wait:
    in al, dx
    test al, 0x20
    jz df_uart_wait

    mov dx, 0x3F8
    mov al, bl
    out dx, al

    pop edx
    pop ebx
    pop eax
    ret

df_puthex32:
    push eax
    push ebx
    push ecx
    push edx

    mov ecx, 8
df_puthex32_loop:
    mov ebx, eax
    shr ebx, 28
    and ebx, 0x0F
    mov dl, [hex_digits + ebx]
    mov al, dl
    call df_putc
    shl eax, 4
    dec ecx
    jnz df_puthex32_loop

    pop edx
    pop ecx
    pop ebx
    pop eax
    ret

isr_df_entry:
    cli
    mov eax, esp

    mov edx, [eax]
    mov ecx, [eax + 4]
    mov ebx, [eax + 8]
    mov esi, [eax + 12]

    mov edi, [eax + 16]
    mov ebp, [eax + 20]

    mov esp, emergency_stack_top

    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    test ebx, 3
    jz isr_df_no_user
    push ebp
    push edi
isr_df_no_user:

    mov al, 'D'
    call df_putc
    mov al, 'F'
    call df_putc
    mov al, ' '
    call df_putc

    mov al, 'e'
    call df_putc
    mov al, 'i'
    call df_putc
    mov al, 'p'
    call df_putc
    mov al, '='
    call df_putc
    mov eax, ecx
    call df_puthex32

    mov al, ' '
    call df_putc

    mov al, 'c'
    call df_putc
    mov al, 'r'
    call df_putc
    mov al, '2'
    call df_putc
    mov al, '='
    call df_putc
    mov eax, cr2
    call df_puthex32

    mov al, 10
    call df_putc

    cli
df_halt:
    hlt
    jmp df_halt

isr_common:
    pusha
    push ds
    push es
    push fs
    push gs
    
    mov ax, 0x10 
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    
    mov eax, esp
    push eax   
    call isr_handler
    pop eax
    
    pop gs
    pop fs
    pop es
    pop ds
    popa
    add esp, 8 
    iret

macro gen_isr n {
    public isr_stub_#n
    isr_stub_#n:
        if n = 8
            jmp isr_df_entry
        end if
        if n = 8 | (n >= 10 & n <= 14) | n = 17
        else
            push 0
        end if
        push n
        jmp isr_common
}

ISR_LIST equ 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19, \
             20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35,36,37,38,39, \
             40,41,42,43,44,45,46,47

macro make_isr_code [id] {
    forward
        gen_isr id
}

match items, ISR_LIST {
    make_isr_code items
}

gen_isr 0x80
gen_isr 0xFF
gen_isr 0xF0
gen_isr 0xF1
gen_isr 0xA1
gen_isr 0xA2

section '.data' writeable

hex_digits db '0123456789ABCDEF'

section '.bss' writeable

emergency_stack rb 8192
emergency_stack_top:

macro make_isr_table [id] {
    forward
        dd isr_stub_#id
}

isr_stub_table:
match items, ISR_LIST {
    make_isr_table items
}

section '.note.GNU-stack'