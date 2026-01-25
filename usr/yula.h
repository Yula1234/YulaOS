// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#ifndef YULA_USER_LIB_H
#define YULA_USER_LIB_H

#include <stdint.h>
#include <stdarg.h> 
#include <stddef.h>

#include <lib/syscall.h>
#include <lib/string.h>
#include <lib/stdlib.h>
#include <lib/stdio.h>

#define YULA_EVENT_NONE       0
#define YULA_EVENT_MOUSE_MOVE 1
#define YULA_EVENT_MOUSE_DOWN 2
#define YULA_EVENT_MOUSE_UP   3
#define YULA_EVENT_KEY_DOWN   4
#define YULA_EVENT_RESIZE     5

typedef struct {
    int type;
    int arg1;
    int arg2;
    int arg3;
} yula_event_t;

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t stride;
    uint32_t bpp;
    uint32_t size_bytes;
} fb_info_t;

typedef struct {
    int32_t x;
    int32_t y;
    int32_t w;
    int32_t h;
} __attribute__((packed)) fb_rect_t;

typedef struct {
    const void* src;
    uint32_t src_stride;
    const fb_rect_t* rects;
    uint32_t rect_count;
} __attribute__((packed)) fb_present_req_t;

typedef struct {
    int32_t x;
    int32_t y;
    int32_t buttons;
} mouse_state_t;


static inline void signal(int sig, void* handler) {
    syscall(17, sig, (int)handler, 0);
}

static inline void sigreturn() {
    syscall(18, 0, 0, 0);
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

void* malloc(size_t size);
void free(void* ptr);
void* calloc(size_t nelem, size_t elsize);
void* realloc(void* ptr, size_t size);

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

static inline int pipe_try_read(int fd, void* buf, uint32_t size) {
    return syscall(44, fd, (int)buf, (int)size);
}

static inline int pipe_try_write(int fd, const void* buf, uint32_t size) {
    return syscall(45, fd, (int)buf, (int)size);
}

static inline int kbd_try_read(char* out) {
    return syscall(46, (int)out, 0, 0);
}

static inline int ipc_listen(const char* name) {
    return syscall(47, (int)name, 0, 0);
}

static inline int ipc_accept(int listen_fd, int out_fds[2]) {
    return syscall(48, listen_fd, (int)out_fds, 0);
}

static inline int ipc_connect(const char* name, int out_fds[2]) {
    return syscall(49, (int)name, (int)out_fds, 0);
}

#define MAP_SHARED  1
#define MAP_PRIVATE 2

static inline int shm_create(uint32_t size) {
    return syscall(43, (int)size, 0, 0);
}

static inline int shm_create_named(const char* name, uint32_t size) {
    return syscall(51, (int)name, (int)size, 0);
}

static inline int shm_open_named(const char* name) {
    return syscall(52, (int)name, 0, 0);
}

static inline int shm_unlink_named(const char* name) {
    return syscall(53, (int)name, 0, 0);
}

static inline int futex_wait(volatile uint32_t* uaddr, uint32_t expected) {
    return syscall(54, (int)uaddr, (int)expected, 0);
}

static inline int futex_wake(volatile uint32_t* uaddr, uint32_t max_wake) {
    return syscall(55, (int)uaddr, (int)max_wake, 0);
}

static inline void* mmap(int fd, uint32_t size, int flags) {
    return (void*)syscall(31, fd, size, flags);
}

static inline int munmap(void* addr, uint32_t length) {
    return syscall(32, (int)addr, length, 0);
}

typedef struct {
    uint32_t type; // 1=FILE, 2=DIR
    uint32_t size;
} __attribute__((packed)) stat_t;

typedef struct {
    uint32_t inode;
    uint32_t type;
    uint32_t size;
    char name[60];
} __attribute__((packed)) yfs_dirent_info_t;

typedef struct {
    uint32_t total_blocks;
    uint32_t free_blocks;
    uint32_t block_size;
} __attribute__((packed)) fs_info_t;

static inline int stat(const char* path, stat_t* buf) {
    return syscall(33, (int)path, (int)buf, 0);
}

static inline int getdents(int fd, void* buf, uint32_t size) {
    return syscall(38, fd, (int)buf, (int)size);
}

static inline int fstatat(int dirfd, const char* name, stat_t* buf) {
    return syscall(39, dirfd, (int)name, (int)buf);
}

static inline int get_fs_info(fs_info_t* buf) {
    return syscall(34, (int)buf, 0, 0);
}

static inline int spawn_process(const char* path, int argc, char** argv) {
    return syscall(36, (int)path, argc, (int)argv);
}

static inline int waitpid(int pid, int* status) {
    return syscall(37, pid, (int)status, 0);
}

static inline void* map_framebuffer(void) {
    uint32_t r = (uint32_t)syscall(40, 0, 0, 0);
    if (r == 0) return 0;
    return (void*)(uintptr_t)r;
}

static inline int fb_acquire(void) {
    return syscall(41, 0, 0, 0);
}

static inline int fb_release(void) {
    return syscall(42, 0, 0, 0);
}

static inline int fb_present(const void* src, uint32_t src_stride, const fb_rect_t* rects, uint32_t rect_count) {
    fb_present_req_t req;
    req.src = src;
    req.src_stride = src_stride;
    req.rects = rects;
    req.rect_count = rect_count;
    return syscall(50, (int)(uintptr_t)&req, 0, 0);
}

#endif