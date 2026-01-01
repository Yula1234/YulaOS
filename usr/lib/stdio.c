// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "syscall.h"

static FILE _stdin  = { .fd = 0, .flags = 1 };
static FILE _stdout = { .fd = 1, .flags = 2 };
static FILE _stderr = { .fd = 2, .flags = 2 };

FILE* stdin  = &_stdin;
FILE* stdout = &_stdout;
FILE* stderr = &_stderr;

int open(const char* path, int flags) {
    return syscall(3, (int)path, flags, 0);
}

int read(int fd, void* buf, uint32_t size) {
    return syscall(4, fd, (int)buf, size);
}

int write(int fd, const void* buf, uint32_t size) {
    return syscall(5, fd, (int)buf, size);
}

int close(int fd) {
    return syscall(6, fd, 0, 0);
}


void print(const char* s) {
    write(1, s, strlen(s));
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
    char c, sign, tmp[66];
    const char* digits = "0123456789abcdefghijklmnopqrstuvwxyz";
    int i;

    if (type & 16) digits = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    if (type & 4) size--;
    if (type & 2) {
        if (base == 16 && (type & 8)) size -= 2;
    }
    
    i = 0;
    if (num == 0) tmp[i++] = '0';
    else {
        if (base == 10 && num < 0) { sign = '-'; num = -num; } else sign = 0;
        unsigned long unum = (unsigned long)num;
        if (base == 10 && sign) unum = (unsigned long)num;

        while (unum != 0) {
            tmp[i++] = digits[unum % base];
            unum /= base;
        }
        if (sign) tmp[i++] = sign;
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
    else if (strchr(mode, 'a')) flags = 1;
    
    int fd = open(filename, flags);
    if (fd < 0) return NULL;
    
    FILE* f = (FILE*)malloc(sizeof(FILE));
    if (!f) { close(fd); return NULL; }
    
    f->fd = fd;
    f->flags = flags;
    f->error = 0;
    f->eof = 0;
    return f;
}

int fclose(FILE* stream) {
    if (!stream) return -1;
    int res = close(stream->fd);
    if (stream != stdin && stream != stdout && stream != stderr) {
        free(stream);
    }
    return res;
}

size_t fread(void* ptr, size_t size, size_t nmemb, FILE* stream) {
    if (!stream) return 0;
    size_t bytes_req = size * nmemb;
    int res = read(stream->fd, ptr, bytes_req);
    if (res < 0) { stream->error = 1; return 0; }
    if (res == 0) { stream->eof = 1; return 0; }
    return res / size;
}

size_t fwrite(const void* ptr, size_t size, size_t nmemb, FILE* stream) {
    if (!stream) return 0;
    size_t bytes_req = size * nmemb;
    int res = write(stream->fd, ptr, bytes_req);
    if (res < 0) { stream->error = 1; return 0; }
    return res / size;
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
    int c;
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