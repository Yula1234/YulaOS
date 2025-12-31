// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#ifndef YULA_USER_LIB_H
#define YULA_USER_LIB_H

#include <stdint.h>
#include <stdarg.h> 
#include <stddef.h>

#include <lib/syscall.h>
#include <lib/string.h>

#define YULA_EVENT_NONE       0
#define YULA_EVENT_MOUSE_MOVE 1
#define YULA_EVENT_MOUSE_DOWN 2
#define YULA_EVENT_MOUSE_UP   3
#define YULA_EVENT_KEY_DOWN   4

typedef struct {
    int type;
    int arg1;
    int arg2;
    int arg3;
} yula_event_t;


static inline void exit(int code) {
    syscall(0, code, 0, 0);
    while(1);
}

static inline void signal(int sig, void* handler) {
    __asm__ volatile("int $0x80" : : "a"(17), "b"(sig), "c"(handler));
}

static inline void sigreturn() {
    __asm__ volatile("int $0x80" : : "a"(18));
}

static inline int getpid() {
    return syscall(2, 0, 0, 0);
}

static inline void sleep(int ms) {
    syscall(7, ms, 0, 0);
}

static inline void* sbrk(int incr) {
    return (void*)syscall(8, incr, 0, 0);
}

void* malloc(uint32_t size);
void free(void* ptr);
void* calloc(size_t nelem, size_t elsize);
void* realloc(void* ptr, size_t size);

void print_dec(int n);

int open(const char* path, int flags);

int read(int fd, void* buf, uint32_t size);

int write(int fd, const void* buf, uint32_t size);

int close(int fd);

size_t strlen(const char* s);

void print(const char* s);
void print_hex(uint32_t n);
void printf(const char* fmt, ...);
void vprintf(const char* fmt, va_list args);
int sprintf(char* str, const char* fmt, ...);
int vsprintf(char* str, const char* fmt, va_list args);


static inline void usleep(uint32_t us) {
    syscall(11, us, 0, 0);
}

static inline int create_window(int w, int h, const char* title) {
    return syscall(20, w, h, (int)title);
}

static inline void* map_window(int win_id) {
    return (void*)syscall(21, win_id, 0, 0);
}

static inline void update_window(int win_id) {
    syscall(22, win_id, 0, 0);
}

static inline int get_event(int win_id, yula_event_t* ev) {
    return syscall(23, win_id, (int)ev, 0);
}

static inline int clipboard_copy(const char* text) {
    return syscall(25, (int)text, strlen(text), 0);
}

static inline int clipboard_paste(char* buf, int max_len) {
    return syscall(26, (int)buf, max_len, 0);
}

static inline void set_term_mode(int mode) {
    syscall(27, mode, 0, 0);
}

static inline void set_console_color(uint32_t fg, uint32_t bg) {
    syscall(28, (int)fg, (int)bg, 0);
}

static inline int pipe(int fds[2]) {
    return syscall(29, (int)fds, 0, 0);
}

static inline int dup2(int oldfd, int newfd) {
    return syscall(30, oldfd, newfd, 0);
}

static inline void* mmap(int fd, uint32_t size, int flags) {
    return (void*)syscall(31, fd, size, flags);
}

static inline int munmap(void* addr, uint32_t length) {
    return syscall(32, (int)addr, length, 0);
}

int atoi(const char* str);

typedef struct {
    uint32_t type; // 1=FILE, 2=DIR
    uint32_t size;
} __attribute__((packed)) stat_t;

typedef struct {
    uint32_t total_blocks;
    uint32_t free_blocks;
    uint32_t block_size;
} __attribute__((packed)) fs_info_t;

static inline int stat(const char* path, stat_t* buf) {
    return syscall(33, (int)path, (int)buf, 0);
}

static inline int get_fs_info(fs_info_t* buf) {
    return syscall(34, (int)buf, 0, 0);
}

static inline int rename(const char* old_path, const char* new_path) {
    return syscall(35, (int)old_path, (int)new_path, 0);
}

#endif