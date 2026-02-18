#ifndef LIB_CPP_ATOMIC_H
#define LIB_CPP_ATOMIC_H

#include <stddef.h>
#include <stdint.h>

namespace kernel {

enum class memory_order : int {
    relaxed = __ATOMIC_RELAXED,
    consume = __ATOMIC_CONSUME,
    acquire = __ATOMIC_ACQUIRE,
    release = __ATOMIC_RELEASE,
    acq_rel = __ATOMIC_ACQ_REL,
    seq_cst = __ATOMIC_SEQ_CST,
};

inline void atomic_thread_fence(memory_order order) noexcept {
    __atomic_thread_fence(static_cast<int>(order));
}

inline void atomic_signal_fence(memory_order order) noexcept {
    __atomic_signal_fence(static_cast<int>(order));
}

inline void cpu_relax() noexcept {
#if defined(__i386__) || defined(__x86_64__)
    __asm__ volatile("pause" ::: "memory");
#else
    __asm__ volatile("" ::: "memory");
#endif
}

template<typename T>
struct is_pointer {
    static constexpr bool value = false;
};

template<typename T>
struct is_pointer<T*> {
    static constexpr bool value = true;
};

template<typename T>
struct is_integral {
    static constexpr bool value = false;
};

template<>
struct is_integral<bool> {
    static constexpr bool value = true;
};

template<>
struct is_integral<char> {
    static constexpr bool value = true;
};

template<>
struct is_integral<signed char> {
    static constexpr bool value = true;
};

template<>
struct is_integral<unsigned char> {
    static constexpr bool value = true;
};

template<>
struct is_integral<short> {
    static constexpr bool value = true;
};

template<>
struct is_integral<unsigned short> {
    static constexpr bool value = true;
};

template<>
struct is_integral<int> {
    static constexpr bool value = true;
};

template<>
struct is_integral<unsigned int> {
    static constexpr bool value = true;
};

template<>
struct is_integral<long> {
    static constexpr bool value = true;
};

template<>
struct is_integral<unsigned long> {
    static constexpr bool value = true;
};

template<>
struct is_integral<long long> {
    static constexpr bool value = true;
};

template<>
struct is_integral<unsigned long long> {
    static constexpr bool value = true;
};

template<typename T>
inline constexpr bool is_integral_v = is_integral<T>::value;

template<typename T>
inline constexpr bool is_pointer_v = is_pointer<T>::value;

template<typename T>
class atomic {
public:
    atomic() = default;

    constexpr atomic(T value) noexcept
        : value_(value) {
    }

    atomic(const atomic&) = delete;
    atomic& operator=(const atomic&) = delete;

    atomic& operator=(T desired) noexcept {
        store(desired);
        return *this;
    }

    bool is_lock_free() const noexcept {
        return __atomic_is_lock_free(sizeof(T), &value_) != 0;
    }

    void store(T desired, memory_order order = memory_order::seq_cst) noexcept {
        __atomic_store_n(&value_, desired, static_cast<int>(order));
    }

    T load(memory_order order = memory_order::seq_cst) const noexcept {
        return __atomic_load_n(&value_, static_cast<int>(order));
    }

    operator T() const noexcept {
        return load();
    }

    T exchange(T desired, memory_order order = memory_order::seq_cst) noexcept {
        return __atomic_exchange_n(&value_, desired, static_cast<int>(order));
    }

    bool compare_exchange_weak(
        T& expected,
        T desired,
        memory_order success,
        memory_order failure
    ) noexcept {
        return __atomic_compare_exchange_n(
            &value_,
            &expected,
            desired,
            true,
            static_cast<int>(success),
            static_cast<int>(failure)
        );
    }

    bool compare_exchange_strong(
        T& expected,
        T desired,
        memory_order success,
        memory_order failure
    ) noexcept {
        return __atomic_compare_exchange_n(
            &value_,
            &expected,
            desired,
            false,
            static_cast<int>(success),
            static_cast<int>(failure)
        );
    }

    bool compare_exchange_weak(
        T& expected,
        T desired,
        memory_order order = memory_order::seq_cst
    ) noexcept {
        return compare_exchange_weak(expected, desired, order, order);
    }

    bool compare_exchange_strong(
        T& expected,
        T desired,
        memory_order order = memory_order::seq_cst
    ) noexcept {
        return compare_exchange_strong(expected, desired, order, order);
    }

    template<typename U = T>
    U fetch_add(
        U arg,
        memory_order order = memory_order::seq_cst
    ) noexcept {
        static_assert(is_integral_v<U> || is_pointer_v<U>);
        return __atomic_fetch_add(&value_, arg, static_cast<int>(order));
    }

    template<typename U = T>
    U fetch_sub(
        U arg,
        memory_order order = memory_order::seq_cst
    ) noexcept {
        static_assert(is_integral_v<U> || is_pointer_v<U>);
        return __atomic_fetch_sub(&value_, arg, static_cast<int>(order));
    }

    template<typename U = T>
    U fetch_and(
        U arg,
        memory_order order = memory_order::seq_cst
    ) noexcept {
        static_assert(is_integral_v<U>);
        return __atomic_fetch_and(&value_, arg, static_cast<int>(order));
    }

    template<typename U = T>
    U fetch_or(
        U arg,
        memory_order order = memory_order::seq_cst
    ) noexcept {
        static_assert(is_integral_v<U>);
        return __atomic_fetch_or(&value_, arg, static_cast<int>(order));
    }

    template<typename U = T>
    U fetch_xor(
        U arg,
        memory_order order = memory_order::seq_cst
    ) noexcept {
        static_assert(is_integral_v<U>);
        return __atomic_fetch_xor(&value_, arg, static_cast<int>(order));
    }

    template<typename U = T>
    U operator++() noexcept {
        static_assert(is_integral_v<U>);
        return fetch_add(1) + 1;
    }

    template<typename U = T>
    U operator++(int) noexcept {
        static_assert(is_integral_v<U>);
        return fetch_add(1);
    }

    template<typename U = T>
    U operator--() noexcept {
        static_assert(is_integral_v<U>);
        return fetch_sub(1) - 1;
    }

    template<typename U = T>
    U operator--(int) noexcept {
        static_assert(is_integral_v<U>);
        return fetch_sub(1);
    }

private:
    alignas(T) mutable T value_{};

    static_assert(is_integral_v<T>);
};

template<typename T>
class atomic<T*> {
public:
    atomic() = default;

    constexpr atomic(T* value) noexcept
        : value_(value) {
    }

    atomic(const atomic&) = delete;
    atomic& operator=(const atomic&) = delete;

    atomic& operator=(T* desired) noexcept {
        store(desired);
        return *this;
    }

    bool is_lock_free() const noexcept {
        return __atomic_is_lock_free(sizeof(T*), &value_) != 0;
    }

    void store(T* desired, memory_order order = memory_order::seq_cst) noexcept {
        __atomic_store_n(&value_, desired, static_cast<int>(order));
    }

    T* load(memory_order order = memory_order::seq_cst) const noexcept {
        return __atomic_load_n(&value_, static_cast<int>(order));
    }

    operator T*() const noexcept {
        return load();
    }

    T* exchange(T* desired, memory_order order = memory_order::seq_cst) noexcept {
        return __atomic_exchange_n(&value_, desired, static_cast<int>(order));
    }

    bool compare_exchange_weak(
        T*& expected,
        T* desired,
        memory_order success,
        memory_order failure
    ) noexcept {
        return __atomic_compare_exchange_n(
            &value_,
            &expected,
            desired,
            true,
            static_cast<int>(success),
            static_cast<int>(failure)
        );
    }

    bool compare_exchange_strong(
        T*& expected,
        T* desired,
        memory_order success,
        memory_order failure
    ) noexcept {
        return __atomic_compare_exchange_n(
            &value_,
            &expected,
            desired,
            false,
            static_cast<int>(success),
            static_cast<int>(failure)
        );
    }

    bool compare_exchange_weak(
        T*& expected,
        T* desired,
        memory_order order = memory_order::seq_cst
    ) noexcept {
        return compare_exchange_weak(expected, desired, order, order);
    }

    bool compare_exchange_strong(
        T*& expected,
        T* desired,
        memory_order order = memory_order::seq_cst
    ) noexcept {
        return compare_exchange_strong(expected, desired, order, order);
    }

    T* fetch_add(
        ptrdiff_t delta,
        memory_order order = memory_order::seq_cst
    ) noexcept {
        return __atomic_fetch_add(&value_, delta, static_cast<int>(order));
    }

    T* fetch_sub(
        ptrdiff_t delta,
        memory_order order = memory_order::seq_cst
    ) noexcept {
        return __atomic_fetch_sub(&value_, delta, static_cast<int>(order));
    }

private:
    alignas(T*) mutable T* value_{};
};

template<typename T>
inline T atomic_load(
    const atomic<T>& v,
    memory_order order = memory_order::seq_cst
) noexcept {
    return v.load(order);
}

template<typename T>
inline void atomic_store(
    atomic<T>& v,
    T desired,
    memory_order order = memory_order::seq_cst
) noexcept {
    v.store(desired, order);
}

template<typename T>
inline T atomic_exchange(
    atomic<T>& v,
    T desired,
    memory_order order = memory_order::seq_cst
) noexcept {
    return v.exchange(desired, order);
}

template<typename T, typename Predicate>
inline void spin_wait_until(
    const atomic<T>& v,
    Predicate predicate,
    memory_order order = memory_order::acquire
) noexcept {
    while (!predicate(v.load(order))) {
        cpu_relax();
    }
}

template<typename T>
inline void spin_wait_equals(
    const atomic<T>& v,
    T expected,
    memory_order order = memory_order::acquire
) noexcept {
    while (v.load(order) != expected) {
        cpu_relax();
    }
}

}

#endif
