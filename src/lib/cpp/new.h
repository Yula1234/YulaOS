#ifndef LIB_CPP_NEW_H
#define LIB_CPP_NEW_H

#include <stddef.h>

namespace kernel {

struct nothrow_t {
    explicit nothrow_t() = default;
};

inline constexpr nothrow_t nothrow;

}

void* operator new(size_t size);
void* operator new[](size_t size);

void operator delete(void* ptr) noexcept;
void operator delete[](void* ptr) noexcept;

void operator delete(void* ptr, size_t size) noexcept;
void operator delete[](void* ptr, size_t size) noexcept;

void* operator new(size_t size, const kernel::nothrow_t& tag) noexcept;
void* operator new[](size_t size, const kernel::nothrow_t& tag) noexcept;

void operator delete(void* ptr, const kernel::nothrow_t& tag) noexcept;
void operator delete[](void* ptr, const kernel::nothrow_t& tag) noexcept;

inline void* operator new(size_t size, void* place) noexcept {
    (void)size;
    return place;
}

inline void* operator new[](size_t size, void* place) noexcept {
    (void)size;
    return place;
}

inline void operator delete(void* ptr, void* place) noexcept {
    (void)ptr;
    (void)place;
}

inline void operator delete[](void* ptr, void* place) noexcept {
    (void)ptr;
    (void)place;
}

#if __cpp_aligned_new

namespace std {

enum class align_val_t : size_t {};

}

void* operator new(size_t size, std::align_val_t align);
void* operator new[](size_t size, std::align_val_t align);

void operator delete(void* ptr, std::align_val_t align) noexcept;
void operator delete[](void* ptr, std::align_val_t align) noexcept;

void operator delete(void* ptr, size_t size, std::align_val_t align) noexcept;
void operator delete[](void* ptr, size_t size, std::align_val_t align) noexcept;

void* operator new(size_t size, std::align_val_t align, const kernel::nothrow_t& tag) noexcept;
void* operator new[](size_t size, std::align_val_t align, const kernel::nothrow_t& tag) noexcept;

void operator delete(void* ptr, std::align_val_t align, const kernel::nothrow_t& tag) noexcept;
void operator delete[](void* ptr, std::align_val_t align, const kernel::nothrow_t& tag) noexcept;

#endif

#endif
