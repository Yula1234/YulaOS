#ifndef LIB_CPP_UTILITY_H
#define LIB_CPP_UTILITY_H

#include <stddef.h>

namespace kernel {

template<typename T>
struct RemoveReference {
    using Type = T;
};

template<typename T>
struct RemoveReference<T&> {
    using Type = T;
};

template<typename T>
struct RemoveReference<T&&> {
    using Type = T;
};

template<typename T>
using RemoveReferenceT = typename RemoveReference<T>::Type;

template<typename T>
constexpr RemoveReferenceT<T>&& move(T&& value) noexcept {
    return static_cast<RemoveReferenceT<T>&&>(value);
}

template<typename T>
constexpr T&& forward(RemoveReferenceT<T>& value) noexcept {
    return static_cast<T&&>(value);
}

template<typename T>
constexpr T&& forward(RemoveReferenceT<T>&& value) noexcept {
    return static_cast<T&&>(value);
}

}

#endif
