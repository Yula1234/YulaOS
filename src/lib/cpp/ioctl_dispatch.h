#ifndef LIB_CPP_IOCTL_DISPATCH_H
#define LIB_CPP_IOCTL_DISPATCH_H

#include <stdint.h>
#include <stddef.h>

#include <yos/ioctl.h>

namespace kernel {

class IoctlDispatcher {
public:
    using HandlerFn = int (*)(void* ctx, uint32_t req, void* arg);

    struct Entry {
        uint32_t req;
        HandlerFn fn;
    };

    template<size_t N>
    explicit IoctlDispatcher(const Entry (&entries)[N])
        : entries_(entries),
          count_(N) {
        static_assert(N > 0u, "IoctlDispatcher requires at least one entry");

        sorted_ = is_sorted(entries);
    }

    int dispatch(void* ctx, uint32_t req, void* arg) const {
        const Entry* e = sorted_ ? find_sorted(req) : find_linear(req);

        if (!e || !e->fn) {
            return -1;
        }

        return e->fn(ctx, req, arg);
    }

    static bool validate_arg(void* arg, uint32_t req, size_t expected_size, size_t expected_align) {
        if (!arg) {
            return false;
        }

        const uint32_t req_size = _YOS_IOC_SIZE(req);

        if (req_size != (uint32_t)expected_size) {
            return false;
        }

        const uintptr_t p = (uintptr_t)arg;

        if (expected_align > 0u && (p % expected_align) != 0u) {
            return false;
        }

        return true;
    }

    template<typename T>
    static T* arg_as(void* arg, uint32_t req) {
        if (!validate_arg(arg, req, sizeof(T), alignof(T))) {
            return nullptr;
        }

        return (T*)arg;
    }

    template<typename T>
    static const T* arg_as_const(void* arg, uint32_t req) {
        if (!validate_arg(arg, req, sizeof(T), alignof(T))) {
            return nullptr;
        }

        return (const T*)arg;
    }

    template<typename Ctx, typename T, int (*Fn)(Ctx&, T&)>
    static int adapt_inout(void* ctx, uint32_t req, void* arg) {
        T* a = arg_as<T>(arg, req);

        if (!a || !ctx) {
            return -1;
        }

        return Fn(*(Ctx*)ctx, *a);
    }

    template<typename Ctx, typename T, int (*Fn)(Ctx&, const T&)>
    static int adapt_in(void* ctx, uint32_t req, void* arg) {
        const T* a = arg_as_const<T>(arg, req);

        if (!a || !ctx) {
            return -1;
        }

        return Fn(*(Ctx*)ctx, *a);
    }

    template<typename Ctx, typename T, int (*Fn)(Ctx&, T)>
    static int adapt_value_in(void* ctx, uint32_t req, void* arg) {
        const T* a = arg_as_const<T>(arg, req);

        if (!a || !ctx) {
            return -1;
        }

        return Fn(*(Ctx*)ctx, *a);
    }

    template<typename Ctx, int (*Fn)(Ctx&)>
    static int adapt_noarg(void* ctx, uint32_t req, void* arg) {
        (void)req;

        if (arg) {
            return -1;
        }

        if (!ctx) {
            return -1;
        }

        return Fn(*(Ctx*)ctx);
    }

private:
    const Entry* find_sorted(uint32_t req) const {
        size_t l = 0u;
        size_t r = count_;

        while (l < r) {
            const size_t m = l + (r - l) / 2u;
            const uint32_t mid = entries_[m].req;

            if (mid == req) {
                return &entries_[m];
            }

            if (mid < req) {
                l = m + 1u;
            } else {
                r = m;
            }
        }

        return nullptr;
    }

    const Entry* find_linear(uint32_t req) const {
        for (size_t i = 0u; i < count_; i++) {
            if (entries_[i].req == req) {
                return &entries_[i];
            }
        }

        return nullptr;
    }

    template<size_t N>
    static constexpr bool is_sorted(const Entry (&entries)[N]) {
        for (size_t i = 1u; i < N; i++) {
            if (entries[i - 1u].req >= entries[i].req) {
                return false;
            }
        }

        return true;
    }

private:
    const Entry* entries_;
    size_t count_;
    
    bool sorted_ = false;
};

}

#endif
