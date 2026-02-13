#ifndef YOS_NETD_CORE_H
#define YOS_NETD_CORE_H

#include <stdint.h>
#include <stddef.h>

namespace netd {

template <typename T>
struct Span {
    T* data;
    uint32_t size;

    constexpr Span() : data(nullptr), size(0) {}
    constexpr Span(T* p, uint32_t n) : data(p), size(n) {}

    constexpr T* begin() const { return data; }
    constexpr T* end() const { return data + size; }
};

template <typename T, uint32_t Cap>
class StaticVec {
public:
    constexpr StaticVec() : m_size(0) {}

    constexpr uint32_t size() const { return m_size; }
    constexpr uint32_t capacity() const { return Cap; }

    T* data() { return m_data; }
    const T* data() const { return m_data; }

    T& operator[](uint32_t i) { return m_data[i]; }
    const T& operator[](uint32_t i) const { return m_data[i]; }

    T* begin() { return m_data; }
    T* end() { return m_data + m_size; }
    const T* begin() const { return m_data; }
    const T* end() const { return m_data + m_size; }

    bool push_back(const T& v) {
        if (m_size >= Cap) {
            return false;
        }

        m_data[m_size] = v;
        m_size++;
        return true;
    }

    void erase_unordered(uint32_t i) {
        if (i >= m_size) {
            return;
        }

        m_size--;
        if (i != m_size) {
            m_data[i] = m_data[m_size];
        }
    }

private:
    T m_data[Cap];
    uint32_t m_size;
};

struct UniqueFd {
    int fd;

    UniqueFd() : fd(-1) {}
    explicit UniqueFd(int v) : fd(v) {}

    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;

    UniqueFd(UniqueFd&& other) : fd(other.fd) {
        other.fd = -1;
    }
    UniqueFd& operator=(UniqueFd&& other) {
        if (this != &other) {
            reset();

            fd = other.fd;
            other.fd = -1;
        }
        return *this;
    }

    ~UniqueFd() {
        reset();
    }

    int get() const { return fd; }
    int release() {
        int r = fd;

        fd = -1;
        return r;
    }

    void reset(int v = -1);
};

class PipePair {
public:
    PipePair();

    PipePair(const PipePair&) = delete;
    PipePair& operator=(const PipePair&) = delete;

    PipePair(PipePair&&) = default;
    PipePair& operator=(PipePair&&) = default;

    bool create();

    int read_fd() const { return m_r.get(); }
    int write_fd() const { return m_w.get(); }

    void signal() const;
    void drain() const;

private:
    UniqueFd m_r;
    UniqueFd m_w;
};

}

#endif
