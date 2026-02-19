#include <kernel/ksyms.h>

#include <stdint.h>

extern const ksym_t ksyms_table[];
extern const uint32_t ksyms_count;

__attribute__((no_instrument_function))
const char* ksyms_resolve(uint32_t addr, uint32_t* out_sym_addr) {
    if (out_sym_addr) {
        *out_sym_addr = 0;
    }

    if (ksyms_count == 0) {
        return 0;
    }

    uint32_t lo = 0;
    uint32_t hi = ksyms_count;

    while (lo < hi) {
        const uint32_t mid = lo + (hi - lo) / 2u;
        const uint32_t a = ksyms_table[mid].addr;

        if (a == addr) {
            if (out_sym_addr) {
                *out_sym_addr = a;
            }
            return ksyms_table[mid].name;
        }

        if (a < addr) {
            lo = mid + 1u;
        } else {
            hi = mid;
        }
    }

    if (lo == 0) {
        return 0;
    }

    const uint32_t idx = lo - 1u;
    if (out_sym_addr) {
        *out_sym_addr = ksyms_table[idx].addr;
    }

    return ksyms_table[idx].name;
}
