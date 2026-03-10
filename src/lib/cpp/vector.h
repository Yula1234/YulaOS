#ifndef LIB_CPP_VECTOR_H
#define LIB_CPP_VECTOR_H

#include <stddef.h>
#include <stdint.h>

#include <kernel/panic.h>
#include <lib/compiler.h>
#include <lib/string.h>

#include <lib/cpp/new.h>
#include <lib/cpp/utility.h>

namespace kernel {

#ifndef __STDCPP_DEFAULT_NEW_ALIGNMENT__
#define __STDCPP_DEFAULT_NEW_ALIGNMENT__ alignof(max_align_t)
#endif

template<typename T>
class Vector {
public:
    using value_type = T;
    using size_type = size_t;
    using difference_type = ptrdiff_t;

    using reference = T&;
    using const_reference = const T&;

    using pointer = T*;
    using const_pointer = const T*;

    using iterator = T*;
    using const_iterator = const T*;

    Vector() = default;

    explicit Vector(size_type count) {
        resize(count);
    }

    Vector(size_type count, const T& value) {
        resize(count, value);
    }

    Vector(const Vector& other) {
        copy_from(other);
    }

    Vector& operator=(const Vector& other) {
        if (this == &other) {
            return *this;
        }

        Vector tmp(other);
        swap(tmp);

        return *this;
    }

    Vector(Vector&& other) noexcept {
        move_from(other);
    }

    Vector& operator=(Vector&& other) noexcept {
        if (this == &other) {
            return *this;
        }

        reset();
        move_from(other);

        return *this;
    }

    ~Vector() {
        reset();
    }

    [[nodiscard]] bool empty() const noexcept {
        return size_ == 0;
    }

    [[nodiscard]] size_type size() const noexcept {
        return size_;
    }

    [[nodiscard]] size_type capacity() const noexcept {
        return capacity_;
    }

    [[nodiscard]] pointer data() noexcept {
        return data_;
    }

    [[nodiscard]] const_pointer data() const noexcept {
        return data_;
    }

    iterator begin() noexcept {
        return data_;
    }

    const_iterator begin() const noexcept {
        return data_;
    }

    const_iterator cbegin() const noexcept {
        return data_;
    }

    iterator end() noexcept {
        return data_ + size_;
    }

    const_iterator end() const noexcept {
        return data_ + size_;
    }

    const_iterator cend() const noexcept {
        return data_ + size_;
    }

    reference front() {
        if (kernel::unlikely(size_ == 0)) {
            panic("Vector::front on empty");
        }

        return data_[0];
    }

    const_reference front() const {
        return data_[0];
    }

    reference back() {
        return data_[size_ - 1];
    }

    const_reference back() const {
        return data_[size_ - 1];
    }

    reference operator[](size_type i) noexcept {
        return data_[i];
    }

    const_reference operator[](size_type i) const noexcept {
        return data_[i];
    }

    reference at(size_type i) {
        if (kernel::unlikely(i >= size_)) {
            panic("Vector::at out of bounds");
        }

        return data_[i];
    }

    const_reference at(size_type i) const {
        if (kernel::unlikely(i >= size_)) {
            panic("Vector::at out of bounds");
        }

        return data_[i];
    }

    void clear() noexcept {
        destroy_range_(data_, data_ + size_);
        size_ = 0;
    }

    void pop_back() {
        data_[size_ - 1].~T();
        size_--;
    }

    void reserve(size_type new_capacity) {
        if (new_capacity <= capacity_) {
            return;
        }

        reallocate_(new_capacity);
    }

    void shrink_to_fit() {
        if (size_ == capacity_) {
            return;
        }

        if (size_ == 0) {
            reset_storage_();
            return;
        }

        reallocate_(size_);
    }

    void resize(size_type new_size) {
        if (new_size < size_) {
            destroy_range_(data_ + new_size, data_ + size_);
            size_ = new_size;
            return;
        }

        if (new_size > size_) {
            ensure_capacity_for_(new_size - size_);

            for (size_type i = size_; i < new_size; i++) {
                new (data_ + i) T();
            }

            size_ = new_size;
        }
    }

    void resize(size_type new_size, const T& value) {
        if (new_size < size_) {
            destroy_range_(data_ + new_size, data_ + size_);
            size_ = new_size;
            return;
        }

        if (new_size > size_) {
            ensure_capacity_for_(new_size - size_);

            for (size_type i = size_; i < new_size; i++) {
                new (data_ + i) T(value);
            }

            size_ = new_size;
        }
    }

    void push_back(const T& value) {
        emplace_back(value);
    }

    void push_back(T&& value) {
        emplace_back(kernel::move(value));
    }

    template<typename... Args>
    reference emplace_back(Args&&... args) {
        ensure_capacity_for_(1);

        new (data_ + size_) T(kernel::forward<Args>(args)...);
        size_++;

        return back();
    }

    iterator insert(const_iterator pos, const T& value) {
        return emplace(pos, value);
    }

    iterator insert(const_iterator pos, T&& value) {
        return emplace(pos, kernel::move(value));
    }

    template<typename... Args>
    iterator emplace(const_iterator pos, Args&&... args) {
        const size_type index = index_from_const_it_(pos);

        ensure_capacity_for_(1);

        pointer place = data_ + index;

        if (index == size_) {
            new (place) T(kernel::forward<Args>(args)...);
            size_++;
            return place;
        }

        move_tail_right_by_one_(index);

        new (place) T(kernel::forward<Args>(args)...);
        size_++;

        return place;
    }

    iterator erase(const_iterator pos) {
        const size_type index = index_from_const_it_(pos);
        if (kernel::unlikely(index >= size_)) {
            panic("Vector::erase out of bounds");
        }

        move_range_left_(index + 1, size_, 1);
        size_--;

        return data_ + index;
    }

    iterator erase(const_iterator first, const_iterator last) {
        const size_type a = index_from_const_it_(first);
        const size_type b = index_from_const_it_(last);

        if (kernel::unlikely(a > b || b > size_)) {
            panic("Vector::erase range invalid");
        }

        const size_type count = b - a;
        if (count == 0) {
            return data_ + a;
        }

        move_range_left_(b, size_, count);
        size_ -= count;

        return data_ + a;
    }

    void swap(Vector& other) noexcept {
        pointer tmp_data = data_;
        size_type tmp_size = size_;
        size_type tmp_capacity = capacity_;

        data_ = other.data_;
        size_ = other.size_;
        capacity_ = other.capacity_;

        other.data_ = tmp_data;
        other.size_ = tmp_size;
        other.capacity_ = tmp_capacity;
    }

    friend bool operator==(const Vector& a, const Vector& b) {
        if (a.size_ != b.size_) {
            return false;
        }

        for (size_type i = 0; i < a.size_; i++) {
            if (!(a.data_[i] == b.data_[i])) {
                return false;
            }
        }

        return true;
    }

    friend bool operator!=(const Vector& a, const Vector& b) {
        return !(a == b);
    }

    friend bool operator<(const Vector& a, const Vector& b) {
        const size_type n = a.size_ < b.size_ ? a.size_ : b.size_;

        for (size_type i = 0; i < n; i++) {
            if (a.data_[i] < b.data_[i]) {
                return true;
            }

            if (b.data_[i] < a.data_[i]) {
                return false;
            }
        }

        return a.size_ < b.size_;
    }

    friend bool operator>(const Vector& a, const Vector& b) {
        return b < a;
    }

    friend bool operator<=(const Vector& a, const Vector& b) {
        return !(b < a);
    }

    friend bool operator>=(const Vector& a, const Vector& b) {
        return !(a < b);
    }

private:
    static constexpr bool trivially_relocatable_ =
        __is_trivially_copyable(T) && __is_trivially_destructible(T);

    static pointer allocate_(size_type count) {
        if (count == 0) {
            return nullptr;
        }

        const size_type bytes = sizeof(T) * count;

#if __cpp_aligned_new
        if constexpr (alignof(T) > __STDCPP_DEFAULT_NEW_ALIGNMENT__) {
            return static_cast<pointer>(::operator new(bytes, static_cast<std::align_val_t>(alignof(T))));
        }
#endif

        return static_cast<pointer>(::operator new(bytes));
    }

    static void deallocate_(pointer ptr, size_type count) noexcept {
        if (!ptr) {
            return;
        }

        (void)count;

#if __cpp_aligned_new
        if constexpr (alignof(T) > __STDCPP_DEFAULT_NEW_ALIGNMENT__) {
            ::operator delete(ptr, static_cast<std::align_val_t>(alignof(T)));
            return;
        }
#endif

        ::operator delete(ptr);
    }

    static void destroy_range_(pointer first, pointer last) noexcept {
        if (kernel::unlikely(first == nullptr || last == nullptr)) {
            return;
        }

        if constexpr (__is_trivially_destructible(T)) {
            (void)first;
            (void)last;
            return;
        }

        for (pointer it = first; it != last; ++it) {
            it->~T();
        }
    }

    size_type index_from_const_it_(const_iterator it) const {
        if (kernel::unlikely(it < data_ || it > data_ + size_)) {
            panic("Vector iterator out of range");
        }

        return static_cast<size_type>(it - data_);
    }

    void ensure_capacity_for_(size_type additional) {
        if (additional == 0) {
            return;
        }

        if (kernel::unlikely(size_ > SIZE_MAX - additional)) {
            panic("Vector size overflow");
        }

        const size_type needed = size_ + additional;
        if (needed <= capacity_) {
            return;
        }

        size_type new_capacity = capacity_ == 0 ? 1 : capacity_;
        while (new_capacity < needed) {
            if (kernel::unlikely(new_capacity > SIZE_MAX / 2)) {
                new_capacity = needed;
                break;
            }

            new_capacity *= 2;
        }

        reallocate_(new_capacity);
    }

    void reallocate_(size_type new_capacity) {
        pointer new_data = allocate_(new_capacity);

        if (size_ != 0) {
            relocate_construct_(new_data, data_, size_);
        }

        destroy_range_(data_, data_ + size_);
        deallocate_(data_, capacity_);

        data_ = new_data;
        capacity_ = new_capacity;
    }

    static void relocate_construct_(pointer dst, pointer src, size_type count) {
        if (count == 0) {
            return;
        }

        if constexpr (trivially_relocatable_) {
            memcpy(dst, src, sizeof(T) * count);
            return;
        }

        for (size_type i = 0; i < count; i++) {
            new (dst + i) T(kernel::move(src[i]));
        }
    }

    void move_tail_right_by_one_(size_type index) {
        if (kernel::unlikely(size_ == 0 || index >= size_)) {
            return;
        }

        if constexpr (trivially_relocatable_) {
            memmove(data_ + index + 1, data_ + index, sizeof(T) * (size_ - index));
            return;
        }

        new (data_ + size_) T(kernel::move(data_[size_ - 1]));

        for (size_type i = size_ - 1; i > index; i--) {
            data_[i] = kernel::move(data_[i - 1]);
        }

        data_[index].~T();
    }

    void move_range_left_(size_type from, size_type to, size_type by) {
        if (by == 0) {
            return;
        }

        if (kernel::unlikely(from > to || by > to)) {
            panic("Vector::move_range_left invalid range");
        }

        if (from == to) {
            destroy_range_(data_ + (to - by), data_ + to);
            return;
        }

        if constexpr (trivially_relocatable_) {
            memmove(data_ + (from - by), data_ + from, sizeof(T) * (to - from));
            return;
        }

        for (size_type i = from; i < to; i++) {
            data_[i - by] = kernel::move(data_[i]);
        }

        destroy_range_(data_ + (to - by), data_ + to);
    }

    void reset_storage_() noexcept {
        deallocate_(data_, capacity_);

        data_ = nullptr;
        capacity_ = 0;
    }

    void reset() noexcept {
        clear();
        reset_storage_();
    }

    void move_from(Vector& other) noexcept {
        data_ = other.data_;
        size_ = other.size_;
        capacity_ = other.capacity_;

        other.data_ = nullptr;
        other.size_ = 0;
        other.capacity_ = 0;
    }

    void copy_from(const Vector& other) {
        if (other.size_ == 0) {
            return;
        }

        data_ = allocate_(other.size_);
        capacity_ = other.size_;

        for (size_type i = 0; i < other.size_; i++) {
            new (data_ + i) T(other.data_[i]);
        }

        size_ = other.size_;
    }

private:
    pointer data_ = nullptr;
    size_type size_ = 0;
    size_type capacity_ = 0;
};

} // namespace kernel

#endif
