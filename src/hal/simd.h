#ifndef HAL_SIMD_H
#define HAL_SIMD_H

void kernel_init_simd() {
    uint32_t cr0;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 &= ~(1 << 2);
    cr0 &= ~(1 << 3);
    cr0 |= (1 << 1); 
    __asm__ volatile("mov %0, %%cr0" : : "r"(cr0));

    uint32_t cr4;
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= (1 << 9);  
    cr4 |= (1 << 10); 
    __asm__ volatile("mov %0, %%cr4" : : "r"(cr4));
}

#endif