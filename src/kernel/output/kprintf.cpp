#include <kernel/output/kprintf.h>
#include <kernel/output/console.h>

#include <lib/cpp/lock_guard.h>

#include <lib/types.h>
#include <stddef.h>
#include <stdint.h>

namespace kernel::output {
namespace {

static kernel::SpinLock g_kprintf_lock;

class ConsoleSink {
public:
    ConsoleSink() = default;

    ConsoleSink(const ConsoleSink&) = delete;
    ConsoleSink& operator=(const ConsoleSink&) = delete;

    ConsoleSink(ConsoleSink&&) = delete;
    ConsoleSink& operator=(ConsoleSink&&) = delete;

    void putc(char c) {
        console_putc(c);
        written_++;
    }

    void repeat(char c, int n) {
        for (int i = 0; i < n; i++) {
            putc(c);
        }
    }

    void write(const char* s, size_t len) {
        for (size_t i = 0; i < len; i++) {
            putc(s[i]);
        }
    }

    int written() const {
        return written_;
    }

private:
    int written_ = 0;
};

enum class Length {
    Default,
    Char,
    Short,
    Long,
    LongLong,
    Size,
    PtrDiff,
    IntMax,
};

struct Spec {
    bool left = false;
    bool plus = false;
    bool space = false;
    bool alt = false;
    bool zero = false;

    int width = -1;
    int precision = -1;

    Length length = Length::Default;
    char conv = '\0';
};

static bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

static int parse_int(const char*& p) {
    int v = 0;

    while (is_digit(*p)) {
        v = (v * 10) + (*p - '0');
        p++;
    }

    return v;
}

static Spec parse_spec(const char*& p, va_list& ap) {
    Spec s;

    for (;;) {
        char c = *p;

        if (c == '-') {
            s.left = true;
        } else if (c == '+') {
            s.plus = true;
        } else if (c == ' ') {
            s.space = true;
        } else if (c == '#') {
            s.alt = true;
        } else if (c == '0') {
            s.zero = true;
        } else {
            break;
        }

        p++;
    }

    if (*p == '*') {
        p++;
        s.width = va_arg(ap, int);

        if (s.width < 0) {
            s.width = -s.width;
            s.left = true;
        }
    } else if (is_digit(*p)) {
        s.width = parse_int(p);
    }

    if (*p == '.') {
        p++;

        if (*p == '*') {
            p++;
            s.precision = va_arg(ap, int);
        } else if (is_digit(*p)) {
            s.precision = parse_int(p);
        } else {
            s.precision = 0;
        }

        if (s.precision < 0) {
            s.precision = -1;
        }
    }

    if (p[0] == 'h' && p[1] == 'h') {
        s.length = Length::Char;
        p += 2;
    } else if (p[0] == 'h') {
        s.length = Length::Short;
        p += 1;
    } else if (p[0] == 'l' && p[1] == 'l') {
        s.length = Length::LongLong;
        p += 2;
    } else if (p[0] == 'l') {
        s.length = Length::Long;
        p += 1;
    } else if (p[0] == 'z') {
        s.length = Length::Size;
        p += 1;
    } else if (p[0] == 't') {
        s.length = Length::PtrDiff;
        p += 1;
    } else if (p[0] == 'j') {
        s.length = Length::IntMax;
        p += 1;
    }

    s.conv = *p;
    if (*p != '\0') {
        p++;
    }

    return s;
}

static uint64_t read_u(const Spec& s, va_list& ap) {
    switch (s.length) {
    case Length::Char:
        return static_cast<unsigned char>(va_arg(ap, unsigned int));
    case Length::Short:
        return static_cast<unsigned short>(va_arg(ap, unsigned int));
    case Length::Long:
        return va_arg(ap, unsigned long);
    case Length::LongLong:
        return va_arg(ap, unsigned long long);
    case Length::Size:
        return va_arg(ap, size_t);
    case Length::PtrDiff:
        return static_cast<uint64_t>(va_arg(ap, ptrdiff_t));
    case Length::IntMax:
        return va_arg(ap, uintmax_t);
    case Length::Default:
    default:
        return va_arg(ap, unsigned int);
    }
}

static int64_t read_i(const Spec& s, va_list& ap) {
    switch (s.length) {
    case Length::Char:
        return static_cast<signed char>(va_arg(ap, int));
    case Length::Short:
        return static_cast<short>(va_arg(ap, int));
    case Length::Long:
        return va_arg(ap, long);
    case Length::LongLong:
        return va_arg(ap, long long);
    case Length::Size:
        return static_cast<int64_t>(va_arg(ap, ssize_t));
    case Length::PtrDiff:
        return static_cast<int64_t>(va_arg(ap, ptrdiff_t));
    case Length::IntMax:
        return va_arg(ap, intmax_t);
    case Length::Default:
    default:
        return va_arg(ap, int);
    }
}

static int utoa_rev(uint64_t v, unsigned base, bool upper, char* out, int cap) {
    const char* digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";

    if (cap <= 0) {
        return 0;
    }

    int n = 0;

    if (v == 0) {
        out[n++] = '0';
        return n;
    }

    while (v != 0 && n < cap) {
        uint64_t q = 0;
        uint32_t r = 0;

        {
            uint64_t rem = 0;
            uint64_t quot = 0;

            for (int bit = 63; bit >= 0; bit--) {
                rem = (rem << 1) | ((v >> bit) & 1u);

                if (rem >= base) {
                    rem -= base;
                    quot |= (1ull << bit);
                }
            }

            q = quot;
            r = static_cast<uint32_t>(rem);
        }

        out[n++] = digits[r];
        v = q;
    }

    return n;
}

static int cstr_len(const char* s) {
    int n = 0;

    while (s[n] != '\0') {
        n++;
    }

    return n;
}

static void emit_str(ConsoleSink& out, const Spec& s, const char* str) {
    if (!str) {
        str = "(null)";
    }

    int len = cstr_len(str);
    if (s.precision >= 0 && len > s.precision) {
        len = s.precision;
    }

    int pad = 0;
    if (s.width > len) {
        pad = s.width - len;
    }

    if (!s.left) {
        out.repeat(' ', pad);
    }

    out.write(str, static_cast<size_t>(len));

    if (s.left) {
        out.repeat(' ', pad);
    }
}

static void emit_char(ConsoleSink& out, const Spec& s, char c) {
    int pad = 0;
    if (s.width > 1) {
        pad = s.width - 1;
    }

    if (!s.left) {
        out.repeat(' ', pad);
    }

    out.putc(c);

    if (s.left) {
        out.repeat(' ', pad);
    }
}

static void emit_u(
    ConsoleSink& out,
    const Spec& s,
    uint64_t v,
    unsigned base,
    bool upper,
    const char* prefix,
    int prefix_len
) {
    char rev[64];
    int digits = utoa_rev(v, base, upper, rev, static_cast<int>(sizeof(rev)));

    if (s.precision == 0 && v == 0) {
        digits = 0;
    }

    int prec = s.precision;
    if (prec < 0) {
        prec = 0;
    }

    int zeroes = (digits < prec) ? (prec - digits) : 0;
    int payload = prefix_len + zeroes + digits;

    int pad = 0;
    if (s.width > payload) {
        pad = s.width - payload;
    }

    bool zero_pad = s.zero && !s.left && s.precision < 0;

    if (!s.left && !zero_pad) {
        out.repeat(' ', pad);
    }

    if (prefix && prefix_len > 0) {
        out.write(prefix, static_cast<size_t>(prefix_len));
    }

    if (!s.left && zero_pad) {
        out.repeat('0', pad);
    }

    out.repeat('0', zeroes);

    for (int i = digits - 1; i >= 0; i--) {
        out.putc(rev[i]);
    }

    if (s.left) {
        out.repeat(' ', pad);
    }
}

static void emit_i(ConsoleSink& out, const Spec& s, int64_t v) {
    char sign_ch = '\0';
    uint64_t mag = 0;

    if (v < 0) {
        sign_ch = '-';
        mag = 0u - static_cast<uint64_t>(v);
    } else {
        if (s.plus) {
            sign_ch = '+';
        } else if (s.space) {
            sign_ch = ' ';
        }

        mag = static_cast<uint64_t>(v);
    }

    char sign_buf[1];
    const char* prefix = nullptr;
    int prefix_len = 0;

    if (sign_ch != '\0') {
        sign_buf[0] = sign_ch;
        prefix = sign_buf;
        prefix_len = 1;
    }

    emit_u(out, s, mag, 10u, false, prefix, prefix_len);
}

static int kvprintf_locked(ConsoleSink& out, const char* fmt, va_list ap);

}

int kvprintf(const char* fmt, va_list ap) {
    kernel::SpinLockSafeGuard guard(g_kprintf_lock);

    ConsoleSink out;
    return kvprintf_locked(out, fmt, ap);
}

int kprintf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = kvprintf(fmt, ap);
    va_end(ap);
    return n;
}

}

extern "C" int kvprintf(const char* fmt, va_list ap) {
    return kernel::output::kvprintf(fmt, ap);
}

extern "C" int kprintf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = kernel::output::kvprintf(fmt, ap);
    va_end(ap);
    return n;
}

namespace kernel::output {
namespace {

static int kvprintf_locked(ConsoleSink& out, const char* fmt, va_list ap) {
    if (!fmt) {
        return 0;
    }

    for (const char* p = fmt; *p != '\0';) {
        if (*p != '%') {
            out.putc(*p);
            p++;
            continue;
        }

        p++;
        if (*p == '\0') {
            break;
        }

        if (*p == '%') {
            out.putc('%');
            p++;
            continue;
        }

        Spec s = parse_spec(p, ap);

        if (s.conv == '\0') {
            break;
        }

        switch (s.conv) {
        case 'c': {
            emit_char(out, s, static_cast<char>(va_arg(ap, int)));
            break;
        }
        case 's': {
            emit_str(out, s, va_arg(ap, const char*));
            break;
        }
        case 'd':
        case 'i': {
            emit_i(out, s, read_i(s, ap));
            break;
        }
        case 'u': {
            emit_u(out, s, read_u(s, ap), 10u, false, nullptr, 0);
            break;
        }
        case 'o': {
            uint64_t v = read_u(s, ap);

            if (s.alt && v == 0 && s.precision == 0) {
                Spec os = s;
                os.precision = 1;
                emit_u(out, os, 0, 8u, false, nullptr, 0);
                break;
            }

            const char* prefix = nullptr;
            int prefix_len = 0;

            char pfx[1];
            if (s.alt && v != 0) {
                pfx[0] = '0';
                prefix = pfx;
                prefix_len = 1;
            }

            emit_u(out, s, v, 8u, false, prefix, prefix_len);
            break;
        }
        case 'x':
        case 'X': {
            bool upper = (s.conv == 'X');
            uint64_t v = read_u(s, ap);

            const char* prefix = nullptr;
            int prefix_len = 0;

            char pfx[2];
            if (s.alt && v != 0) {
                pfx[0] = '0';
                pfx[1] = upper ? 'X' : 'x';
                prefix = pfx;
                prefix_len = 2;
            }

            emit_u(out, s, v, 16u, upper, prefix, prefix_len);
            break;
        }
        case 'p': {
            Spec ps = s;
            ps.alt = true;

            void* ptr = va_arg(ap, void*);
            uintptr_t v = reinterpret_cast<uintptr_t>(ptr);

            const char pfx[] = "0x";
            emit_u(out, ps, static_cast<uint64_t>(v), 16u, false, pfx, 2);
            break;
        }
        default:
            out.putc('%');
            out.putc(s.conv);
            break;
        }
    }

    return out.written();
}

}
}
