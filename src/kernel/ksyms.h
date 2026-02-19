#ifndef KERNEL_KSYMS_H
#define KERNEL_KSYMS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t addr;
    const char* name;
} ksym_t;

extern const ksym_t ksyms_table[];
extern const uint32_t ksyms_count;

const char* ksyms_resolve(uint32_t addr, uint32_t* out_sym_addr);

#ifdef __cplusplus
}
#endif

#endif
