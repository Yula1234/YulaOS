#include <lib/cpp/ctors.h>

#include <stdint.h>

typedef void (*ctor_fn_t)(void);

extern "C" ctor_fn_t __init_array_start[];
extern "C" ctor_fn_t __init_array_end[];

extern "C" uint32_t kernel_end;

static inline int ctor_ptr_is_kernel_text(uint32_t ptr) {
    const uint32_t kernel_base = 0x00100000u;
    const uint32_t kernel_end_addr = (uint32_t)(uintptr_t)&kernel_end;

    if (ptr < kernel_base) {
        return 0;
    }

    return ptr < kernel_end_addr;
}

extern "C" void cpp_call_global_ctors(void) {
    for (ctor_fn_t* fn = __init_array_start; fn < __init_array_end; fn++) {
        const uint32_t fn_ptr = (uint32_t)(uintptr_t)(*fn);

        if (!ctor_ptr_is_kernel_text(fn_ptr)) {
            break;
        }

        (*fn)();
    }
}
