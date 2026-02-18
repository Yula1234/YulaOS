#ifndef LIB_CPP_HASH_TRAITS_H
#define LIB_CPP_HASH_TRAITS_H

#include <stdint.h>

#include <lib/cpp/string.h>

namespace kernel {

template<typename T>
struct HashTraits {
    static uint32_t hash(const T& value) = delete;
};

template<>
struct HashTraits<uint32_t> {
    static uint32_t hash(const uint32_t& value) {
        uint32_t x = value;

        x ^= x >> 16;
        x *= 0x7feb352du;
        x ^= x >> 15;
        x *= 0x846ca68bu;
        x ^= x >> 16;

        return x;
    }
};

template<>
struct HashTraits<kernel::string> {
    static uint32_t hash(const kernel::string& value) {
        return value.hash();
    }
};

}

#endif
