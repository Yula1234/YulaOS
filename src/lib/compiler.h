// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 Yula1234

#ifndef LIB_COMPILER_H
#define LIB_COMPILER_H

#ifdef __cplusplus

namespace kernel {

[[nodiscard]] constexpr inline bool likely(bool value) noexcept {
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_expect(!!value, 1);
#else
    return value;
#endif
}

[[nodiscard]] constexpr inline bool unlikely(bool value) noexcept {
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_expect(!!value, 0);
#else
    return value;
#endif
}

}

#else

#if defined(__GNUC__) || defined(__clang__)
    #define likely(x)       __builtin_expect(!!(x), 1)
    #define unlikely(x)     __builtin_expect(!!(x), 0)
#else
    #define likely(x)       (x)
    #define unlikely(x)     (x)
#endif

#endif

#endif