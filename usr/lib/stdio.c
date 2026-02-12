// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "syscall.h"
#include "pthread.h"

static unsigned char _stdout_wbuf[BUFSIZ];

static FILE _stdin  = { .fd = 0, .flags = 1 };
static FILE _stdout = { .fd = 1, .flags = 2, .wbuf = _stdout_wbuf, .wbuf_size = BUFSIZ, .wbuf_mode = _IOLBF };
static FILE _stderr = { .fd = 2, .flags = 2, .wbuf_mode = _IONBF };

FILE* stdin  = &_stdin;
FILE* stdout = &_stdout;
FILE* stderr = &_stderr;

static pthread_mutex_t stdio_lock = PTHREAD_MUTEX_INITIALIZER;

static void stdio_lock_acquire(void) {
    pthread_mutex_lock(&stdio_lock);
}

static void stdio_lock_release(void) {
    pthread_mutex_unlock(&stdio_lock);
}

static int stdio_flush_wbuf(FILE* stream);
static int fflush_unlocked(FILE* stream);

int open(const char* path, int flags) {
    return syscall(3, (int)path, flags, 0);
}

static int read_unlocked(int fd, void* buf, uint32_t size) {
    if (fd == 0 && stdout && stdout->wbuf_mode == _IOLBF && stdout->wbuf_len) {
        (void)stdio_flush_wbuf(stdout);
    }
    return syscall(4, fd, (int)buf, size);
}

int read(int fd, void* buf, uint32_t size) {
    int res;
    stdio_lock_acquire();
    res = read_unlocked(fd, buf, size);
    stdio_lock_release();
    return res;
}

int write(int fd, const void* buf, uint32_t size) {
    return syscall(5, fd, (int)buf, size);
}

int close(int fd) {
    return syscall(6, fd, 0, 0);
}


static size_t stdio_min_size(size_t a, size_t b) {
    return (a < b) ? a : b;
}

static int stdio_write_loop(FILE* stream, const unsigned char* buf, size_t len, size_t* out_written) {
    size_t total = 0;
    while (total < len) {
        size_t chunk = len - total;
        if (chunk > 0x7FFFFFFFu) chunk = 0x7FFFFFFFu;
        int r = write(stream->fd, buf + total, (uint32_t)chunk);
        if (r <= 0) {
            stream->error = 1;
            if (out_written) *out_written = total;
            return -1;
        }
        total += (size_t)r;
    }
    if (out_written) *out_written = total;
    return 0;
}

static int stdio_flush_wbuf(FILE* stream) {
    if (!stream) return -1;
    if (stream->wbuf_mode == _IONBF) return 0;
    if (!stream->wbuf || stream->wbuf_len == 0) return 0;

    size_t written = 0;
    int rc = stdio_write_loop(stream, stream->wbuf, stream->wbuf_len, &written);

    if (rc == 0) {
        stream->wbuf_len = 0;
        return 0;
    }

    if (written > 0 && written < stream->wbuf_len) {
        memmove(stream->wbuf, stream->wbuf + written, stream->wbuf_len - written);
        stream->wbuf_len -= written;
    } else {
        stream->wbuf_len = 0;
    }

    return rc;
}

static int stdio_ensure_wbuf(FILE* stream) {
    if (!stream) return -1;
    if (stream->wbuf_mode == _IONBF) return 0;
    if (stream->wbuf && stream->wbuf_size) return 0;

    size_t size = stream->wbuf_size ? stream->wbuf_size : (size_t)BUFSIZ;
    if (size == 0) size = 1;

    unsigned char* buf = (unsigned char*)malloc(size);
    if (!buf) {
        stream->wbuf_mode = _IONBF;
        return -1;
    }

    stream->wbuf = buf;
    stream->wbuf_size = size;
    stream->wbuf_len = 0;
    stream->wbuf_owned = 1;

    return 0;
}


void print(const char* s) {
    fputs(s, stdout);
}

void print_dec(int n) {
    char buf[32];
    sprintf(buf, "%d", n);
    print(buf);
}

void print_hex(uint32_t n) {
    char buf[32];
    sprintf(buf, "%x", n);
    print(buf);
}

static char* number(char* str, char* end, long num, int base, int size, int precision, int type) {
    char sign, tmp[66];
    const char* digits = "0123456789abcdefghijklmnopqrstuvwxyz";
    int i;

    if (type & 16) digits = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    sign = 0;
    if (base == 10 && num < 0) {
        sign = '-';
    }
    if (sign) size--;
    if (type & 2) {
        if (base == 16 && (type & 8)) size -= 2;
    }
    
    i = 0;
    if (num == 0) tmp[i++] = '0';
    else {
        unsigned long unum;
        if (sign) {
            unum = 0u - (unsigned long)num;
        } else {
            unum = (unsigned long)num;
        }

        while (unum != 0) {
            tmp[i++] = digits[unum % base];
            unum /= base;
        }
    }

    if (i > precision) precision = i;
    size -= precision;
    
    if (!(type & (2 + 4))) while (size-- > 0 && str < end) *str++ = ' ';
    if (sign && str < end) *str++ = sign;
    
    if (type & 2) {
        while (size-- > 0 && str < end) *str++ = '0';
    }
    
    while (i < precision-- && str < end) *str++ = '0';
    while (i-- > 0 && str < end) *str++ = tmp[i];
    while (size-- > 0 && str < end) *str++ = ' ';

    return str;
}

int vsnprintf(char* buf, size_t size, const char* fmt, va_list args) {
    char* str = buf;
    char* end = buf + size - 1;
    const char* s;
    
    if (size == 0) return 0;

    for (; *fmt; ++fmt) {
        if (*fmt != '%') {
            if (str < end) *str++ = *fmt;
            continue;
        }
        
        int flags = 0;
        fmt++;
        
        while(1) {
            if (*fmt == '-') flags |= 4;
            else if (*fmt == '+') flags |= 1;
            else if (*fmt == ' ') flags |= 32;
            else if (*fmt == '0') flags |= 2;
            else break;
            fmt++;
        }
        
        int field_width = -1;
        if (*fmt >= '0' && *fmt <= '9') {
            field_width = 0;
            while (*fmt >= '0' && *fmt <= '9') {
                field_width = field_width * 10 + (*fmt - '0');
                fmt++;
            }
        } else if (*fmt == '*') {
            fmt++;
            field_width = va_arg(args, int);
            if (field_width < 0) {
                field_width = -field_width;
                flags |= 4;
            }
        }
        
        int precision = -1;
        if (*fmt == '.') {
            fmt++;
            precision = 0;
            if (*fmt >= '0' && *fmt <= '9') {
                while (*fmt >= '0' && *fmt <= '9') {
                    precision = precision * 10 + (*fmt - '0');
                    fmt++;
                }
            } else if (*fmt == '*') {
                fmt++;
                precision = va_arg(args, int);
            }
        }
        
        if (*fmt == 'h' || *fmt == 'l' || *fmt == 'L') fmt++;

        if (*fmt == 's') {
            s = va_arg(args, char*);
            if (!s) s = "(null)";
            int len = strlen(s);
            if (precision >= 0 && len > precision) len = precision;
            
            if (!(flags & 4)) while (len < field_width-- && str < end) *str++ = ' ';
            for (int i = 0; i < len && str < end; ++i) *str++ = *s++;
            while (len < field_width-- && str < end) *str++ = ' ';
        } 
        else if (*fmt == 'd' || *fmt == 'i') {
            str = number(str, end, va_arg(args, int), 10, field_width, precision, flags);
        } 
        else if (*fmt == 'u') {
            str = number(str, end, va_arg(args, unsigned int), 10, field_width, precision, flags);
        }
        else if (*fmt == 'x') {
            str = number(str, end, va_arg(args, unsigned int), 16, field_width, precision, flags);
        } 
        else if (*fmt == 'X') {
            str = number(str, end, va_arg(args, unsigned int), 16, field_width, precision, flags | 16);
        } 
        else if (*fmt == 'p') {
            if (str < end) { *str++ = '0'; }
            if (str < end) { *str++ = 'x'; }
            str = number(str, end, (unsigned long)va_arg(args, void*), 16, field_width, precision, flags);
        }
        else if (*fmt == 'c') {
            if (str < end) *str++ = (unsigned char)va_arg(args, int);
        } 
        else if (*fmt == '%') {
            if (str < end) *str++ = '%';
        } 
        else {
            if (str < end) *str++ = '%';
            if (*fmt && str < end) *str++ = *fmt;
        }
    }
    *str = '\0';
    return str - buf;
}

int snprintf(char* str, size_t size, const char* format, ...) {
    va_list args;
    va_start(args, format);
    int res = vsnprintf(str, size, format, args);
    va_end(args);
    return res;
}

int sprintf(char* str, const char* format, ...) {
    va_list args;
    va_start(args, format);
    int res = vsnprintf(str, 4096, format, args); 
    va_end(args);
    return res;
}

int vsprintf(char* str, const char* format, va_list ap) {
    return vsnprintf(str, 4096, format, ap);
}

int vfprintf(FILE* stream, const char* format, va_list ap) {
    char buf[BUFSIZ];
    int len = vsnprintf(buf, sizeof(buf), format, ap);
    if (len > 0) {
        fwrite(buf, 1, len, stream);
    }
    return len;
}

int fprintf(FILE* stream, const char* format, ...) {
    va_list args;
    va_start(args, format);
    int res = vfprintf(stream, format, args);
    va_end(args);
    return res;
}

int printf(const char* format, ...) {
    va_list args;
    va_start(args, format);
    int res = vfprintf(stdout, format, args);
    va_end(args);
    return res;
}

int vprintf(const char* format, va_list ap) {
    return vfprintf(stdout, format, ap);
}

FILE* fopen(const char* filename, const char* mode) {
    int flags = 0;
    if (strchr(mode, 'w')) flags = 1;
    else if (strchr(mode, 'r')) flags = 0;
    else if (strchr(mode, 'a')) flags = 2;
    
    int fd = open(filename, flags);
    if (fd < 0) return NULL;
    
    FILE* f = (FILE*)malloc(sizeof(FILE));
    if (!f) { close(fd); return NULL; }
    
    f->fd = fd;
    f->flags = flags;
    f->error = 0;
    f->eof = 0;
    f->wbuf = 0;
    f->wbuf_size = BUFSIZ;
    f->wbuf_len = 0;
    f->wbuf_mode = _IOFBF;
    f->wbuf_owned = 0;
    return f;
}

int fclose(FILE* stream) {
    if (!stream) return -1;

    stdio_lock_acquire();
    (void)fflush_unlocked(stream);

    int res = close(stream->fd);
    if (stream != stdin && stream != stdout && stream != stderr) {
        if (stream->wbuf_owned && stream->wbuf) {
            free(stream->wbuf);
            stream->wbuf = 0;
        }
        free(stream);
    }
    stdio_lock_release();
    return res;
}

size_t fread(void* ptr, size_t size, size_t nmemb, FILE* stream) {
    if (!stream) return 0;
    stdio_lock_acquire();
    size_t bytes_req = size * nmemb;
    int res = read_unlocked(stream->fd, ptr, bytes_req);
    if (res < 0) {
        stream->error = 1;
        stdio_lock_release();
        return 0;
    }
    if (res == 0) {
        stream->eof = 1;
        stdio_lock_release();
        return 0;
    }
    size_t out = res / size;
    stdio_lock_release();
    return out;
}

size_t fwrite(const void* ptr, size_t size, size_t nmemb, FILE* stream) {
    if (!stream) return 0;
    stdio_lock_acquire();
    size_t bytes_req = size * nmemb;
    if (bytes_req == 0) {
        stdio_lock_release();
        return 0;
    }

    const unsigned char* p = (const unsigned char*)ptr;
    size_t bytes_done = 0;

    if (stream->wbuf_mode == _IONBF) {
        size_t written = 0;
        if (stdio_write_loop(stream, p, bytes_req, &written) != 0) {
            bytes_done = written;
        } else {
            bytes_done = bytes_req;
        }
        size_t out = size ? (bytes_done / size) : 0;
        stdio_lock_release();
        return out;
    }

    if (stdio_ensure_wbuf(stream) != 0) {
        size_t written = 0;
        if (stdio_write_loop(stream, p, bytes_req, &written) != 0) {
            bytes_done = written;
        } else {
            bytes_done = bytes_req;
        }
        size_t out = size ? (bytes_done / size) : 0;
        stdio_lock_release();
        return out;
    }

    while (bytes_done < bytes_req) {
        size_t remaining = bytes_req - bytes_done;

        size_t chunk = remaining;
        int need_flush_after = 0;

        if (stream->wbuf_mode == _IOLBF) {
            void* nl = memchr(p + bytes_done, '\n', remaining);
            if (nl) {
                chunk = (size_t)((unsigned char*)nl - (p + bytes_done)) + 1;
                need_flush_after = 1;
            }
        }

        if (stream->wbuf_mode == _IOFBF && stream->wbuf_len == 0 && chunk >= stream->wbuf_size) {
            size_t written = 0;
            if (stdio_write_loop(stream, p + bytes_done, chunk, &written) != 0) {
                bytes_done += written;
                size_t out = size ? (bytes_done / size) : 0;
                stdio_lock_release();
                return out;
            }
            bytes_done += chunk;
            continue;
        }

        size_t cpos = 0;
        while (cpos < chunk) {
            if (stream->wbuf_len == stream->wbuf_size) {
                if (stdio_flush_wbuf(stream) != 0) {
                    size_t out = size ? (bytes_done / size) : 0;
                    stdio_lock_release();
                    return out;
                }
            }

            size_t space = stream->wbuf_size - stream->wbuf_len;
            size_t take = stdio_min_size(space, chunk - cpos);

            memcpy(stream->wbuf + stream->wbuf_len, p + bytes_done + cpos, take);
            stream->wbuf_len += take;
            cpos += take;
        }

        bytes_done += chunk;

        if (need_flush_after) {
            if (stdio_flush_wbuf(stream) != 0) {
                size_t out = size ? (bytes_done / size) : 0;
                stdio_lock_release();
                return out;
            }
        }
    }

    size_t out = size ? (bytes_done / size) : 0;
    stdio_lock_release();
    return out;
}

int fputc(int c, FILE* stream) {
    char ch = (char)c;
    if (fwrite(&ch, 1, 1, stream) == 1) return c;
    return EOF;
}

int fgetc(FILE* stream) {
    unsigned char c;
    if (fread(&c, 1, 1, stream) == 1) return c;
    return EOF;
}

int fputs(const char* s, FILE* stream) {
    int len = strlen(s);
    if (fwrite(s, 1, len, stream) == (size_t)len) return len;
    return EOF;
}

char* fgets(char* s, int size, FILE* stream) {
    if (size <= 0) return NULL;
    char* p = s;
    int c = EOF;
    while (--size > 0 && (c = fgetc(stream)) != EOF) {
        *p++ = c;
        if (c == '\n') break;
    }
    *p = '\0';
    if (p == s && c == EOF) return NULL;
    return s;
}

int remove(const char* filename) {
    return syscall(14, (int)filename, 0, 0);
}

int rename(const char* oldname, const char* newname) {
    return syscall(35, (int)oldname, (int)newname, 0);
}

int fflush(FILE* stream) {
    int res;
    stdio_lock_acquire();
    res = fflush_unlocked(stream);
    stdio_lock_release();
    return res;
}

static int fflush_unlocked(FILE* stream) {
    if (!stream) {
        int a = fflush_unlocked(stdout);
        int b = fflush_unlocked(stderr);
        return (a == 0 && b == 0) ? 0 : EOF;
    }
    return (stdio_flush_wbuf(stream) == 0) ? 0 : EOF;
}

int setvbuf(FILE* stream, char* buf, int mode, size_t size) {
    if (!stream) return -1;
    if (mode != _IONBF && mode != _IOLBF && mode != _IOFBF) return -1;

    stdio_lock_acquire();
    (void)fflush_unlocked(stream);

    if (stream->wbuf_owned && stream->wbuf) {
        free(stream->wbuf);
    }

    stream->wbuf = 0;
    stream->wbuf_size = 0;
    stream->wbuf_len = 0;
    stream->wbuf_owned = 0;
    stream->wbuf_mode = mode;

    if (mode == _IONBF) {
        stdio_lock_release();
        return 0;
    }

    if (size == 0) size = BUFSIZ;
    if (size == 0) size = 1;

    if (buf) {
        stream->wbuf = (unsigned char*)buf;
        stream->wbuf_size = size;
        stream->wbuf_owned = 0;
        stdio_lock_release();
        return 0;
    }

    unsigned char* nb = (unsigned char*)malloc(size);
    if (!nb) {
        stream->wbuf_mode = _IONBF;
        stdio_lock_release();
        return -1;
    }
    stream->wbuf = nb;
    stream->wbuf_size = size;
    stream->wbuf_owned = 1;
    stdio_lock_release();
    return 0;
}

void setbuf(FILE* stream, char* buf) {
    if (!stream) return;
    if (!buf) (void)setvbuf(stream, 0, _IONBF, 0);
    else (void)setvbuf(stream, buf, _IOFBF, BUFSIZ);
}
