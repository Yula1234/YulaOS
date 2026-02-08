#include "util.h"

int min(int a, int b) {
    if (a < b) {
        return a;
    }
    return b;
}

int max(int a, int b) {
    if (a > b) {
        return a;
    }
    return b;
}

int is_digit(char c) {
    return (c >= '0' && c <= '9');
}

int is_alpha(char c) {
    if (c >= 'a' && c <= 'z') {
        return 1;
    }
    if (c >= 'A' && c <= 'Z') {
        return 1;
    }
    if (c == '_' || c == '.') {
        return 1;
    }
    return 0;
}

int is_word_char(char c) {
    return is_alpha(c) || is_digit(c);
}

char lower_char(char c) {
    if (c >= 'A' && c <= 'Z') {
        return (char)(c + ('a' - 'A'));
    }
    return c;
}

const char* path_ext(const char* s) {
    if (!s) {
        return 0;
    }

    int n = strlen(s);
    for (int i = n - 1; i >= 0; i--) {
        char c = s[i];
        if (c == '.') {
            return s + i + 1;
        }
        if (c == '/' || c == '\\') {
            break;
        }
    }
    return 0;
}

const char* path_base(const char* s) {
    if (!s) {
        return "";
    }
    const char* last = s;
    for (const char* p = s; *p; p++) {
        if (*p == '/' || *p == '\\') {
            last = p + 1;
        }
    }
    return last;
}

void fmt_title_ellipsis(const char* s, char* out, int out_cap, int max_chars) {
    if (!out || out_cap <= 0) {
        return;
    }
    out[0] = 0;
    if (!s) {
        return;
    }

    int n = (int)strlen(s);
    if (max_chars < 4) {
        max_chars = 4;
    }
    if (max_chars > out_cap - 1) {
        max_chars = out_cap - 1;
    }

    if (n <= max_chars) {
        for (int i = 0; i < n; i++) {
            out[i] = s[i];
        }
        out[n] = 0;
        return;
    }

    out[0] = '.';
    out[1] = '.';
    out[2] = '.';
    int keep = max_chars - 3;
    for (int i = 0; i < keep; i++) {
        out[3 + i] = s[n - keep + i];
    }
    out[max_chars] = 0;
}

void fmt_int(int n, char* buf) {
    if (n == 0) {
        buf[0] = '0';
        buf[1] = 0;
        return;
    }

    int t = n;
    int digits = 0;
    while (t > 0) {
        t /= 10;
        digits++;
    }

    buf[digits] = 0;
    int i = digits;
    while (n > 0) {
        i--;
        buf[i] = (char)('0' + (n % 10));
        n /= 10;
    }
}
