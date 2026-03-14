#include <kernel/symbols/ksyms.h>

#include <stdint.h>

__attribute__((weak)) const ksym_t ksyms_table[] = { { 0u, 0 } };
__attribute__((weak)) const uint32_t ksyms_count = 0;
