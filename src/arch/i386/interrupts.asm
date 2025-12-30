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

section '.data' writeable

macro make_isr_table [id] {
    forward
        dd isr_stub_#id
}

isr_stub_table:
match items, ISR_LIST {
    make_isr_table items
}

section '.note.GNU-stack'