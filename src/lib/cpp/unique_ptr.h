#ifndef LIB_CPP_UNIQUE_PTR_H
#define LIB_CPP_UNIQUE_PTR_H

#include <lib/cpp/new.h>
#include <lib/cpp/utility.h>

namespace kernel {

template<typename T>
struct default_delete {
    void operator()(T* ptr) const noexcept {
        delete ptr;
    }
};

template<typename T>
struct default_delete<T[]> {
    void operator()(T* ptr) const noexcept {
        delete[] ptr;
    }
};

template<typename T, typename Deleter = default_delete<T>>
class unique_ptr {
public:
    unique_ptr() = default;

    explicit unique_ptr(T* ptr) noexcept
        : ptr_(ptr) {
    }

    unique_ptr(T* ptr, Deleter deleter) noexcept
        : ptr_(ptr),
          deleter_(kernel::move(deleter)) {
    }

    unique_ptr(const unique_ptr&) = delete;
    unique_ptr& operator=(const unique_ptr&) = delete;

    unique_ptr(unique_ptr&& other) noexcept
        : ptr_(kernel::move(other.ptr_)),
          deleter_(kernel::move(other.deleter_)) {
        other.ptr_ = nullptr;
    }

    unique_ptr& operator=(unique_ptr&& other) noexcept {
        if (this == &other) {
            return *this;
        }

        reset();

        ptr_ = kernel::move(other.ptr_);
        deleter_ = kernel::move(other.deleter_);

        other.ptr_ = nullptr;

        return *this;
    }

    ~unique_ptr() {
        reset();
    }

    T* get() const noexcept {
        return ptr_;
    }

    T* release() noexcept {
        T* out = ptr_;
        ptr_ = nullptr;
        return out;
    }

    void reset(T* ptr = nullptr) noexcept {
        if (ptr_ == ptr) {
            return;
        }

        if (ptr_) {
            deleter_(ptr_);
        }

        ptr_ = ptr;
    }

    explicit operator bool() const noexcept {
        return ptr_ != nullptr;
    }

    T* operator->() const noexcept {
        return ptr_;
    }

    T& operator*() const noexcept {
        return *ptr_;
    }

private:
    T* ptr_ = nullptr;
    Deleter deleter_{};
};

template<typename T, typename... Args>
unique_ptr<T> make_unique(Args&&... args) {
    return unique_ptr<T>(new (kernel::nothrow) T(kernel::forward<Args>(args)...));
}

}

#endif
