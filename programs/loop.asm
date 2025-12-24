section .data
msg: db "Loop!", 10

section .text
_start:
    mov ecx, 5

loop_start:
    push ecx
    
    mov eax, 5
    mov ebx, 1
    mov ecx, msg
    mov edx, 6
    int 0x80
    
    pop ecx
    dec ecx
    cmp ecx, 0
    jne loop_start

    mov eax, 0
    mov ebx, 0
    int 0x80