/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2026 Yula1234 */

#ifndef LIB_COMPILER_H
#define LIB_COMPILER_H

#define ___always_inline __attribute__((always_inline))
#define ___inline __attribute__((always_inline)) static inline
#define ___noinline __attribute__((noinline))
#define ___unused __attribute__((unused))

#define READ_ONCE(x)       __atomic_load_n(&(x), __ATOMIC_RELAXED)
#define WRITE_ONCE(x, val) __atomic_store_n(&(x), (val), __ATOMIC_RELAXED)

#ifdef __cplusplus /* C++ */

namespace kernel {

[[nodiscard]] ___always_inline constexpr inline bool likely(bool value) noexcept {
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_expect(!!value, 1);
#else
    return value;
#endif
}

[[nodiscard]] ___always_inline constexpr inline bool unlikely(bool value) noexcept {
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_expect(!!value, 0);
#else
    return value;
#endif
}

}

#else /* C */

#define CLASS(name, var) \
    class_##name##_t var __attribute__((cleanup(class_##name##_destructor))) = class_##name##_constructor

#define DEFINE_CLASS(_name, _type, _exit, _init, _init_args...)     \
    typedef _type class_##_name##_t;                                \
    ___inline void class_##_name##_destructor(_type *p)             \
    { _type _T = *p; _exit; }                                       \
    ___inline _type class_##name##_constructor(_init_args)          \
    { _type t = _init; return t; }


#if defined(__GNUC__) || defined(__clang__)
    #define likely(x)       __builtin_expect(!!(x), 1)
    #define unlikely(x)     __builtin_expect(!!(x), 0)
#else
    #define likely(x)       (x)
    #define unlikely(x)     (x)
#endif

#endif

#endif