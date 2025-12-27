format ELF
public smp_trampoline_start
public smp_trampoline_end

section '.rodata'

smp_trampoline_start:
    file 'bin/smp_trampoline.bin'
smp_trampoline_end:

section '.note.GNU-stack'