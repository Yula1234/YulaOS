// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#ifndef _STDLIB_H
#define _STDLIB_H

#include <stddef.h>

void* malloc(size_t size);
void  free(void* ptr);
void* calloc(size_t nelem, size_t elsize);
void* realloc(void* ptr, size_t size);

void exit(int status);
void abort(void);

int   atoi(const char* str);
long  atol(const char* str);
char* itoa(int value, char* str, int base);

long strtol(const char* nptr, char** endptr, int base);
unsigned long strtoul(const char* nptr, char** endptr, int base);

int  abs(int j);
int  rand(void);
void srand(unsigned int seed);

typedef int (*cmp_func_t)(const void*, const void*);
void qsort(void* base, size_t nmemb, size_t size, cmp_func_t compar);

#endif