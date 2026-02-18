#ifndef LIB_CPP_EXPECTED_H
#define LIB_CPP_EXPECTED_H

#include <stddef.h>

#include <lib/cpp/utility.h>

namespace kernel {

template<typename T, typename E>
class Expected {
public:
    Expected(const Expected&) = delete;
    Expected& operator=(const Expected&) = delete;

    Expected(Expected&& other) noexcept {
        move_from(other);
    }

    Expected& operator=(Expected&& other) noexcept {
        if (this == &other) {
            return *this;
        }

        reset();

        move_from(other);

        return *this;
    }

    ~Expected() {
        reset();
    }

    static Expected success(T value) {
        Expected out;

        out.engaged_ = true;
        out.has_value_ = true;

        new (&out.storage_.value) T(kernel::move(value));

        return out;
    }

    static Expected failure(E error) {
        Expected out;

        out.engaged_ = true;
        out.has_value_ = false;

        new (&out.storage_.error) E(kernel::move(error));

        return out;
    }

    explicit operator bool() const {
        return engaged_ && has_value_;
    }

    T& value() {
        return storage_.value;
    }

    const T& value() const {
        return storage_.value;
    }

    E& error() {
        return storage_.error;
    }

    const E& error() const {
        return storage_.error;
    }

private:
    Expected() = default;

    void reset() {
        if (!engaged_) {
            return;
        }

        if (has_value_) {
            storage_.value.~T();
        } else {
            storage_.error.~E();
        }

        engaged_ = false;
    }

    void move_from(Expected& other) {
        engaged_ = other.engaged_;
        has_value_ = other.has_value_;

        if (!engaged_) {
            return;
        }

        if (has_value_) {
            new (&storage_.value) T(kernel::move(other.storage_.value));
        } else {
            new (&storage_.error) E(kernel::move(other.storage_.error));
        }

        other.reset();
    }

private:
    union Storage {
        Storage() {
        }

        ~Storage() {
        }

        T value;
        E error;
    } storage_;

    bool engaged_ = false;
    bool has_value_ = false;
};

template<typename E>
class Expected<void, E> {
public:
    Expected(const Expected&) = delete;
    Expected& operator=(const Expected&) = delete;

    Expected(Expected&& other) noexcept {
        move_from(other);
    }

    Expected& operator=(Expected&& other) noexcept {
        if (this == &other) {
            return *this;
        }

        reset();

        move_from(other);

        return *this;
    }

    ~Expected() {
        reset();
    }

    static Expected success() {
        Expected out;

        out.engaged_ = true;
        out.has_value_ = true;

        return out;
    }

    static Expected failure(E error) {
        Expected out;

        out.engaged_ = true;
        out.has_value_ = false;

        new (&out.storage_.error) E(kernel::move(error));

        return out;
    }

    explicit operator bool() const {
        return engaged_ && has_value_;
    }

    E& error() {
        return storage_.error;
    }

    const E& error() const {
        return storage_.error;
    }

private:
    Expected() = default;

    void reset() {
        if (!engaged_) {
            return;
        }

        if (!has_value_) {
            storage_.error.~E();
        }

        engaged_ = false;
    }

    void move_from(Expected& other) {
        engaged_ = other.engaged_;
        has_value_ = other.has_value_;

        if (!engaged_) {
            return;
        }

        if (!has_value_) {
            new (&storage_.error) E(kernel::move(other.storage_.error));
        }

        other.reset();
    }

private:
    bool engaged_ = false;
    bool has_value_ = false;

    union Storage {
        Storage() {
        }

        ~Storage() {
        }

        E error;
    } storage_;
};

}

#endif
