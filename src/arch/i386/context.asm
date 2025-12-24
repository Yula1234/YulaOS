format ELF

public ctx_switch
public ctx_start
public irq_return

section '.text' executable

ctx_switch:
    push ebp
    push ebx
    push esi
    push edi

    mov eax, [esp+20]
    mov [eax], esp

    mov esp, [esp+24]

    pop edi
    pop esi
    pop ebx
    pop ebp
    ret

ctx_start:
    mov esp, [esp+4]

    pop edi
    pop esi
    pop ebx
    pop ebp
    ret

irq_return:
    mov ax, 0x23 
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    
    iret

section '.note.GNU-stack'