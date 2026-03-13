; SPDX-License-Identifier: GPL-2.0
; Copyright (C) 2025 Yula1234

format ELF
use32

public _start
extrn main

section '.text'

_start:
    
    mov eax, [esp + 4]     ; argc
    mov ecx, [esp + 8]     ; argv
    
    sub esp, 4
    
    push ecx               ; argv
    push eax               ; argc
    
    call main
    
    mov ebx, eax
    mov eax, 0
    int 0x80
    
    hlt

section '.note.GNU-stack'