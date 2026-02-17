#include <kernel/panic.h>
#include <mm/heap.h>

#include <lib/cpp/new.h>
#include <stddef.h>
#include <stdint.h>

namespace {

[[noreturn]] void out_of_memory(size_t size, size_t align) {
    (void)size;
    (void)align;

    panic("C++: out of memory");
    __builtin_unreachable();
}

void* alloc(size_t size) {
    void* ptr = kmalloc(size);
    if (!ptr) {
        out_of_memory(size, 0u);
    }

    return ptr;
}

void* alloc_aligned(size_t size, size_t align) {
    void* ptr = kmalloc_aligned(size, (uint32_t)align);
    if (!ptr) {
        out_of_memory(size, align);
    }

    return ptr;
}

}

void* operator new(size_t size) {
    return alloc(size);
}

void* operator new[](size_t size) {
    return alloc(size);
}

void operator delete(void* ptr) noexcept {
    kfree(ptr);
}

void operator delete[](void* ptr) noexcept {
    kfree(ptr);
}

void operator delete(void* ptr, size_t) noexcept {
    kfree(ptr);
}

void operator delete[](void* ptr, size_t) noexcept {
    kfree(ptr);
}

void* operator new(size_t size, const kernel::nothrow_t&) noexcept {
    return kmalloc(size);
}

void* operator new[](size_t size, const kernel::nothrow_t&) noexcept {
    return kmalloc(size);
}

void operator delete(void* ptr, const kernel::nothrow_t&) noexcept {
    kfree(ptr);
}

void operator delete[](void* ptr, const kernel::nothrow_t&) noexcept {
    kfree(ptr);
}

#if __cpp_aligned_new

void* operator new(size_t size, std::align_val_t align) {
    return alloc_aligned(size, (size_t)align);
}

void* operator new[](size_t size, std::align_val_t align) {
    return alloc_aligned(size, (size_t)align);
}

void operator delete(void* ptr, std::align_val_t) noexcept {
    kfree(ptr);
}

void operator delete[](void* ptr, std::align_val_t) noexcept {
    kfree(ptr);
}

void operator delete(void* ptr, size_t, std::align_val_t) noexcept {
    kfree(ptr);
}

void operator delete[](void* ptr, size_t, std::align_val_t) noexcept {
    kfree(ptr);
}

void* operator new(size_t size, std::align_val_t align, const kernel::nothrow_t&) noexcept {
    return kmalloc_aligned(size, (uint32_t)align);
}

void* operator new[](size_t size, std::align_val_t align, const kernel::nothrow_t&) noexcept {
    return kmalloc_aligned(size, (uint32_t)align);
}

void operator delete(void* ptr, std::align_val_t, const kernel::nothrow_t&) noexcept {
    kfree(ptr);
}

void operator delete[](void* ptr, std::align_val_t, const kernel::nothrow_t&) noexcept {
    kfree(ptr);
}

#endif
