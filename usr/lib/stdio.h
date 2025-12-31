// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#ifndef _STDIO_H
#define _STDIO_H

#include <stddef.h>
#include <stdarg.h>
#include <stdint.h>

#define EOF (-1)

#ifndef NULL
#define NULL ((void*)0)
#endif

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#define BUFSIZ 1024

typedef struct _iobuf {
    int  fd;
    int  flags;
    int  error;
    int  eof;
} FILE;

extern FILE* stdin;
extern FILE* stdout;
extern FILE* stderr;

int open(const char* path, int flags);
int read(int fd, void* buf, uint32_t size);
int write(int fd, const void* buf, uint32_t size);
int close(int fd);

void print(const char* s);
void print_dec(int n);
void print_hex(uint32_t n);

FILE* fopen(const char* filename, const char* mode);
int   fclose(FILE* stream);
size_t fread(void* ptr, size_t size, size_t nmemb, FILE* stream);
size_t fwrite(const void* ptr, size_t size, size_t nmemb, FILE* stream);

#define feof(f)   ((f)->eof)
#define ferror(f) ((f)->error)

int   fputc(int c, FILE* stream);
int   fgetc(FILE* stream);
int   fputs(const char* s, FILE* stream);
char* fgets(char* s, int size, FILE* stream);

#define putc(c, stream) fputc(c, stream)
#define getc(stream)    fgetc(stream)
#define putchar(c)      fputc(c, stdout)
#define getchar()       fgetc(stdin)
#define puts(s)         fputs(s, stdout)

int vsnprintf(char* str, size_t size, const char* format, va_list ap);

int sprintf(char* str, const char* format, ...);
int snprintf(char* str, size_t size, const char* format, ...);
int vsprintf(char* str, const char* format, va_list ap);

int fprintf(FILE* stream, const char* format, ...);
int vfprintf(FILE* stream, const char* format, va_list ap);

int printf(const char* format, ...);
int vprintf(const char* format, va_list ap);

int remove(const char* filename);
int rename(const char* oldname, const char* newname);

#endif