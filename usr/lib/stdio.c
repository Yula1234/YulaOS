// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#include <stdint.h>
#include <stdarg.h> 
#include <stddef.h>

#include "syscall.h"

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

size_t strlen(const char* s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

int strcmp(const char* s1, const char* s2) {
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return *(const unsigned char*)s1 - *(const unsigned char*)s2;
}

int strncmp(const char* s1, const char* s2, size_t n) {
    while (n && *s1 && (*s1 == *s2)) {
        s1++;
        s2++;
        n--;
    }
    if (n == 0) return 0;
    return *(const unsigned char*)s1 - *(const unsigned char*)s2;
}

char* strcpy(char* dest, const char* src) {
    char* d = dest;
    while ((*d++ = *src++));
    return dest;
}

char* strncpy(char* dest, const char* src, size_t n) {
    size_t i;
    for (i = 0; i < n && src[i] != '\0'; i++)
        dest[i] = src[i];
    for ( ; i < n; i++)
        dest[i] = '\0';
    return dest;
}

char* strcat(char* dest, const char* src) {
    char* ptr = dest + strlen(dest);
    while (*src != '\0') {
        *ptr++ = *src++;
    }
    *ptr = '\0';
    return dest;
}

static char* number(char* str, long num, int base, int size, int precision, int type) {
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
        // Handle unsigned for hex
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
    
    if (!(type & (2 + 4))) while (size-- > 0) *str++ = ' ';
    if (sign) *str++ = sign;
    
    if (type & 2) { // Pad zeros
        while (size-- > 0) *str++ = '0';
    }
    
    while (i < precision--) *str++ = '0';
    while (i-- > 0) *str++ = tmp[i];
    while (size-- > 0) *str++ = ' '; 

    return str;
}



void print(const char* s) {
    write(1, s, strlen(s));
}

void print_dec(int n) {
    char buf[16];
    int i = 0;
    if (n == 0) { print("0"); return; }
    int sign = n < 0;
    if (sign) n = -n;
    while (n > 0) {
        buf[i++] = (n % 10) + '0';
        n /= 10;
    }
    if (sign) buf[i++] = '-';
    buf[i] = '\0';
    for (int j = 0; j < i / 2; j++) {
        char t = buf[j];
        buf[j] = buf[i - 1 - j];
        buf[i - 1 - j] = t;
    }
    print(buf);
}

void print_hex(uint32_t n) {
    char* chars = "0123456789ABCDEF";
    char buf[9];
    buf[8] = '\0';
    for (int i = 7; i >= 0; i--) {
        buf[i] = chars[n % 16];
        n /= 16;
    }
    print(buf);
}

void print_hex_raw(uint32_t n) {
    char* chars = "0123456789ABCDEF";
    char buf[16];
    int i = 0;
    
    if (n == 0) {
        print("0");
        return;
    }

    while (n > 0) {
        buf[i++] = chars[n % 16];
        n /= 16;
    }
    
    for (int j = 0; j < i / 2; j++) {
        char t = buf[j];
        buf[j] = buf[i - 1 - j];
        buf[i - 1 - j] = t;
    }
    buf[i] = 0;
    print(buf);
}

void vprintf(const char* fmt, va_list args) {
    char* p = (char*)fmt;
    while (*p) {
        if (*p == '%') {
            p++;
            if (*p == 'd') {
                print_dec(va_arg(args, int));
            } else if (*p == 's') {
                print(va_arg(args, char*));
            } else if (*p == 'x') {
                print_hex_raw(va_arg(args, uint32_t));
            } else if (*p == 'c') {
                char cc = (char)va_arg(args, uint32_t);
                char tmpbuf[2] = {cc, 0};
                write(1, tmpbuf, 1);
            }
            p++;
        } else {
            char temp[2] = {*p, 0};
            print(temp);
            p++;
        }
    }
}


int vsprintf(char* buf, const char* fmt, va_list args) {
    char* str = buf;
    const char* s;
    
    for (; *fmt; ++fmt) {
        if (*fmt != '%') { *str++ = *fmt; continue; }
        
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
        }
        
        if (*fmt == 's') {
            s = va_arg(args, char*);
            if (!s) s = "<NULL>";
            int len = strlen(s);
            if (!(flags & 4)) while (len < field_width--) *str++ = ' ';
            while (*s) *str++ = *s++;
            while (len < field_width--) *str++ = ' ';
        } else if (*fmt == 'd' || *fmt == 'i') {
            str = number(str, va_arg(args, int), 10, field_width, 0, flags);
        } else if (*fmt == 'x') {
            str = number(str, va_arg(args, unsigned int), 16, field_width, 0, flags);
        } else if (*fmt == 'X') {
            str = number(str, va_arg(args, unsigned int), 16, field_width, 0, flags | 16);
        } else if (*fmt == 'c') {
            *str++ = (unsigned char)va_arg(args, int);
        } else if (*fmt == '%') {
            *str++ = '%';
        } else {
            *str++ = '%';
            if (*fmt) *str++ = *fmt;
        }
    }
    *str = '\0';
    return str - buf;
}

int sprintf(char* str, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int i = vsprintf(str, fmt, args);
    va_end(args);
    return i;
}

void printf(const char* fmt, ...) {
    char buf[2048];
    va_list args;
    va_start(args, fmt);
    vsprintf(buf, fmt, args);
    va_end(args);
    print(buf);
}

void* memset(void* dst, int v, uint32_t n) {
    uint8_t* p = (uint8_t*)dst;
    while (n--) *p++ = (uint8_t)v;
    return dst;
}

void* memcpy(void* dest, const void* src, uint32_t n) {
    char* d = (char*)dest;
    const char* s = (const char*)src;
    while (n--) *d++ = *s++;
    return dest;
}

int atoi(const char* str) {
    int res = 0;
    int sign = 1;
    int i = 0;
    
    // Skip spaces
    while(str[i] == ' ') i++;
    
    if(str[i] == '-') {
        sign = -1;
        i++;
    } else if (str[i] == '+') {
        i++;
    }
    
    while(str[i] >= '0' && str[i] <= '9') {
        res = res * 10 + (str[i] - '0');
        i++;
    }
    return res * sign;
}