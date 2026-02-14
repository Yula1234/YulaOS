#ifndef YOS_NETD_NET_INPLACE_H
#define YOS_NETD_NET_INPLACE_H

#include <stdint.h>
#include <stddef.h>

namespace netd {

template <typename T>
class Inplace {
public:
    Inplace() : m_ptr(nullptr) {
    }

    Inplace(const Inplace&) = delete;
    Inplace& operator=(const Inplace&) = delete;

    T* get() {
        return m_ptr;
    }

    const T* get() const {
        return m_ptr;
    }

    T& operator*() {
        return *m_ptr;
    }

    const T& operator*() const {
        return *m_ptr;
    }

    T* operator->() {
        return m_ptr;
    }

    const T* operator->() const {
        return m_ptr;
    }

    explicit operator bool() const {
        return m_ptr != nullptr;
    }

    template <typename... Args>
    T* construct(Args&&... args) {
        m_ptr = new (&m_storage[0]) T((Args&&)args...);
        return m_ptr;
    }

    void destroy() {
        if (!m_ptr) {
            return;
        }

        m_ptr->~T();
        m_ptr = nullptr;
    }

private:
    alignas(T) uint8_t m_storage[sizeof(T)];
    T* m_ptr;
};

}

#endif
