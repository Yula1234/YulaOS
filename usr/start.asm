format ELF
use32

public _start
extrn main

section '.text'

_start:
    ; [esp]     -> Fake return address (0)
    ; [esp+4]   -> argc
    ; [esp+8]   -> argv
    pop eax 
    call main
    
    mov ebx, eax    ; exit code
    mov eax, 0 
    int 0x80        ; exit()
    
    hlt

section '.note.GNU-stack'