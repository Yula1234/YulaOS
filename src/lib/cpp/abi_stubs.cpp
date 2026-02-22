#include <kernel/panic.h>

#include <stdint.h>

extern "C" {

void* __dso_handle = nullptr;

int __cxa_atexit(void (*)(void*), void*, void*) {
    return 0;
}

void __cxa_pure_virtual(void) {
    panic("C++: pure virtual call");
}

int __cxa_guard_acquire(uint64_t* guard) {
    if (!guard) {
        return 0;
    }

    return (*guard == 0u) ? 1 : 0;
}

void __cxa_guard_release(uint64_t* guard) {
    if (!guard) {
        return;
    }

    *guard = 1u;
}

void __cxa_guard_abort(uint64_t*) {
}

}
