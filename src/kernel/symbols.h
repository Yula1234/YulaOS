#ifndef KERNEL_SYMBOLS_H
#define KERNEL_SYMBOLS_H

#include <kernel/boot.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void symbols_init(const multiboot_info_t* mb_info);

const char* symbols_resolve(uint32_t addr, uint32_t* out_sym_addr);

#ifdef __cplusplus
}
#endif

#endif
