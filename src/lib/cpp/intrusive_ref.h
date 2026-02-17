#ifndef LIB_CPP_INTRUSIVE_REF_H
#define LIB_CPP_INTRUSIVE_REF_H

#include <lib/cpp/utility.h>

namespace kernel {

struct AdoptRefTag {
    explicit AdoptRefTag() = default;
};

inline constexpr AdoptRefTag adopt_ref;

template<typename T>
class IntrusiveRef {
public:
    IntrusiveRef() = default;

    IntrusiveRef(T* ptr, AdoptRefTag)
        : ptr_(ptr) {
    }

    static IntrusiveRef adopt(T* ptr) {
        return IntrusiveRef(ptr, adopt_ref);
    }

    static IntrusiveRef from_borrowed(T* ptr) {
        if (!ptr) {
            return {};
        }

        if (!retain(ptr)) {
            return {};
        }

        return adopt(ptr);
    }

    IntrusiveRef(const IntrusiveRef&) = delete;
    IntrusiveRef& operator=(const IntrusiveRef&) = delete;

    IntrusiveRef(IntrusiveRef&& other) noexcept
        : ptr_(kernel::move(other.ptr_)) {
        other.ptr_ = nullptr;
    }

    IntrusiveRef& operator=(IntrusiveRef&& other) noexcept {
        if (this == &other) {
            return *this;
        }

        reset();

        ptr_ = kernel::move(other.ptr_);
        other.ptr_ = nullptr;

        return *this;
    }

    ~IntrusiveRef() {
        reset();
    }

    T* get() const {
        return ptr_;
    }

    explicit operator bool() const {
        return ptr_ != nullptr;
    }

    T* detach() {
        T* out = ptr_;
        ptr_ = nullptr;
        return out;
    }

    void reset() {
        if (!ptr_) {
            return;
        }

        ptr_->release();
        ptr_ = nullptr;
    }

private:
    template<typename U>
    static auto retain_dispatch(U* ptr, int) -> decltype(static_cast<bool>(ptr->retain())) {
        return ptr->retain();
    }

    template<typename U>
    static bool retain_dispatch(U* ptr, long) {
        ptr->retain();
        return true;
    }

    static bool retain(T* ptr) {
        return retain_dispatch(ptr, 0);
    }

    T* ptr_ = nullptr;
};

}

#endif
