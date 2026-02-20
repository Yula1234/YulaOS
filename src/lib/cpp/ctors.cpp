#include <lib/cpp/ctors.h>

#include <stdint.h>

typedef void (*ctor_fn_t)(void);

extern "C" ctor_fn_t __init_array_start[];
extern "C" ctor_fn_t __init_array_end[];

extern "C" void cpp_call_global_ctors(void) {
    for (ctor_fn_t* fn = __init_array_start; fn < __init_array_end; fn++) {
        (*fn)();
    }
}
