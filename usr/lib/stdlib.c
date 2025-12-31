// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#include "stdlib.h"
#include "syscall.h"

void exit(int status) {
    while(1);
}

void abort(void) {
    exit(134);
}

int abs(int j) {
    return (j < 0) ? -j : j;
}

static unsigned int next_rand = 1;

int rand(void) {
    next_rand = next_rand * 1103515245 + 12345;
    return (unsigned int)(next_rand / 65536) % 32768;
}

void srand(unsigned int seed) {
    next_rand = seed;
}

int atoi(const char* str) {
    int res = 0;
    int sign = 1;
    int i = 0;
    
    while (str[i] == ' ' || str[i] == '\t' || str[i] == '\n') i++;
    
    if (str[i] == '-') { sign = -1; i++; }
    else if (str[i] == '+') { i++; }
    
    while (str[i] >= '0' && str[i] <= '9') {
        res = res * 10 + (str[i] - '0');
        i++;
    }
    return res * sign;
}

long atol(const char* str) {
    return (long)atoi(str);
}

char* itoa(int value, char* str, int base) {
    char* rc;
    char* ptr;
    char* low;
    
    if (base < 2 || base > 36) {
        *str = '\0';
        return str;
    }
    
    rc = ptr = str;
    
    if (value < 0 && base == 10) {
        *ptr++ = '-';
    }
    
    low = ptr;
    
    unsigned int num = (value < 0 && base == 10) ? -value : (unsigned int)value;
    
    do {
        *ptr++ = "0123456789abcdefghijklmnopqrstuvwxyz"[num % base];
        num /= base;
    } while (num);
    
    *ptr-- = '\0';
    
    while (low < ptr) {
        char tmp = *low;
        *low++ = *ptr;
        *ptr-- = tmp;
    }
    
    return rc;
}

void qsort(void* base, size_t nmemb, size_t size, cmp_func_t compar) {
    if (nmemb < 2 || size == 0) return;
    
    char* base_ptr = (char*)base;
    char tmp[256];
    
    if (size > sizeof(tmp)) return; 

    for (size_t i = 0; i < nmemb - 1; i++) {
        for (size_t j = 0; j < nmemb - i - 1; j++) {
            char* a = base_ptr + j * size;
            char* b = base_ptr + (j + 1) * size;
            
            if (compar(a, b) > 0) {
                for(size_t k=0; k<size; k++) tmp[k] = a[k];
                for(size_t k=0; k<size; k++) a[k] = b[k];
                for(size_t k=0; k<size; k++) b[k] = tmp[k];
            }
        }
    }
}