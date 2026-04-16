; SPDX-License-Identifier: GPL-2.0
; Copyright (C) 2025 Yula1234

format ELF
section '.text' executable

public isr_stub_table
public load_page_directory
public enable_paging
public sysenter_entry
extrn isr_handler
extrn double_fault_report

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

    push eax
    push ebp
    push edi
    push esi
    push ebx
    push ecx
    push edx
    call double_fault_report

    cli
df_halt:
    hlt
    jmp df_halt

align 4
sysenter_entry:
    push 0x23
    push ecx
    pushfd
    or dword [esp], 0x200
    push 0x1B
    push edx
    push 0x1337
    push 0x80
    
    pusha
    
    push ds
    push es
    push fs
    push gs
    
    mov ax, 0x10 
    mov ds, ax
    mov es, ax
    mov fs, ax
    
    mov eax, esp
    push eax
    cld                   
    call isr_handler
    pop eax
    
    pop gs
    pop fs
    pop es
    pop ds
    
    popa                  
    
    cmp dword [esp + 4], 0x1337
    jne .use_iret
    
    add esp, 8
    pop edx
    add esp, 4
    popfd
    pop ecx
    add esp, 4
    sysexit
    
.use_iret:
    add esp, 8
    iret

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

rept 256 i:0 {
    gen_isr i
}


section '.data' writeable

hex_digits db '0123456789ABCDEF'

section '.bss' writeable

emergency_stack rb 8192
emergency_stack_top:

section '.data' writeable

macro make_isr_table [id] {
    forward
        dd isr_stub_#id
}

isr_stub_table:
rept 256 i:0 {
    dd isr_stub_#i
}

section '.note.GNU-stack'