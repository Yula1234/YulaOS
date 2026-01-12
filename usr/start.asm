; SPDX-License-Identifier: GPL-2.0
; Copyright (C) 2025 Yula1234

format ELF
use32

public _start
extrn main

section '.text'

_start:
    pop eax 
    call main
    
    mov ebx, eax 
    mov eax, 0 
    int 0x80 
    
    hlt

section '.note.GNU-stack'