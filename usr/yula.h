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
#include <lib/pthread.h>
#include <yos/ioctl.h>
#include <yos/proc.h>

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

typedef struct {
    int32_t fd;
    int16_t events;
    int16_t revents;
} __attribute__((packed)) pollfd_t;

#define POLLIN   0x001
#define POLLOUT  0x004
#define POLLERR  0x008
#define POLLHUP  0x010
#define POLLNVAL 0x020


static inline void signal(int sig, void* handler) {
    syscall(15, sig, (int)handler, 0);
}

static inline void sigreturn() {
    syscall(16, 0, 0, 0);
}

static inline int getpid() {
    return syscall(1, 0, 0, 0);
}

static inline int kill(int pid) {
    return syscall(8, pid, 0, 0);
}

static inline void sleep(int ms) {
    syscall(6, ms, 0, 0);
}

static inline void* sbrk(int incr) {
    return (void*)syscall(7, incr, 0, 0);
}

void* malloc(size_t size);
void free(void* ptr);
void* calloc(size_t nelem, size_t elsize);
void* realloc(void* ptr, size_t size);

static inline void usleep(uint32_t us) {
    syscall(9, us, 0, 0);
}

static inline int poll(pollfd_t* fds, uint32_t nfds, int timeout_ms) {
    return syscall(43, (int)(uintptr_t)fds, (int)nfds, (int)timeout_ms);
}

static inline int ioctl(int fd, uint32_t req, void* arg) {
    return syscall(44, fd, (int)req, (int)arg);
}

static inline int clipboard_copy(const char* text) {
    if (!text) {
        return -1;
    }

    const int fd = open("/dev/clipboard", VFS_OPEN_WRITE | VFS_OPEN_TRUNC);
    if (fd < 0) {
        return -1;
    }

    const uint32_t len = (uint32_t)strlen(text);
    const int rc = write(fd, text, len);
    (void)close(fd);

    return rc;
}

static inline int clipboard_paste(char* buf, int max_len) {
    if (!buf || max_len <= 0) {
        return -1;
    }

    const int fd = open("/dev/clipboard", 0);
    if (fd < 0) {
        return -1;
    }

    const uint32_t size = (uint32_t)max_len;
    const int rc = read(fd, buf, size);
    (void)close(fd);

    return rc;
}

static inline void set_term_mode(int mode) {
    syscall(18, mode, 0, 0);
}

static inline int pipe(int fds[2]) {
    return syscall(19, (int)fds, 0, 0);
}

static inline int dup2(int oldfd, int newfd) {
    return syscall(20, oldfd, newfd, 0);
}

static inline int pipe_try_read(int fd, void* buf, uint32_t size) {
    return syscall(31, fd, (int)buf, (int)size);
}

static inline int pipe_try_write(int fd, const void* buf, uint32_t size) {
    return syscall(32, fd, (int)buf, (int)size);
}

static inline int kbd_try_read(char* out) {
    return syscall(33, (int)out, 0, 0);
}

static inline int ipc_listen(const char* name) {
    return syscall(34, (int)name, 0, 0);
}

static inline int ipc_accept(int listen_fd, int out_fds[2]) {
    return syscall(35, listen_fd, (int)out_fds, 0);
}

static inline int ipc_connect(const char* name, int out_fds[2]) {
    return syscall(36, (int)name, (int)out_fds, 0);
}

static inline int chdir(const char* path) {
    return syscall(45, (int)path, 0, 0);
}

static inline int getcwd(char* buf, uint32_t size) {
    return syscall(46, (int)buf, (int)size, 0);
}

static inline int mkdir(const char* path) {
    return syscall(11, (int)(uintptr_t)path, 0, 0);
}

static inline int mkdirat(int dirfd, const char* path) {
    return syscall(53, dirfd, (int)(uintptr_t)path, 0);
}

static inline int unlink(const char* path) {
    return syscall(12, (int)(uintptr_t)path, 0, 0);
}

static inline int unlinkat(int dirfd, const char* path) {
    return syscall(54, dirfd, (int)(uintptr_t)path, 0);
}

static inline uint32_t uptime_ms(void) {
    return (uint32_t)syscall(47, 0, 0, 0);
}

static inline int proc_list(yos_proc_info_t* buf, uint32_t cap) {
    return syscall(48, (int)(uintptr_t)buf, (int)cap, 0);
}

static inline int setsid(void) {
    return syscall(49, 0, 0, 0);
}

static inline int setpgid(uint32_t pgid) {
    return syscall(50, (int)pgid, 0, 0);
}

static inline int setpgid_pid(uint32_t pid, uint32_t pgid) {
    return syscall(50, (int)pid, (int)pgid, 0);
}

static inline uint32_t getpgrp(void) {
    return (uint32_t)syscall(51, 0, 0, 0);
}

#define MAP_SHARED  1
#define MAP_PRIVATE 2

static inline int shm_create(uint32_t size) {
    return syscall(30, (int)size, 0, 0);
}

static inline int shm_create_named(const char* name, uint32_t size) {
    return syscall(38, (int)name, (int)size, 0);
}

static inline int shm_open_named(const char* name) {
    return syscall(39, (int)name, 0, 0);
}

static inline int shm_unlink_named(const char* name) {
    return syscall(40, (int)name, 0, 0);
}

static inline int futex_wait(volatile uint32_t* uaddr, uint32_t expected) {
    return syscall(41, (int)uaddr, (int)expected, 0);
}

static inline int futex_wake(volatile uint32_t* uaddr, uint32_t max_wake) {
    return syscall(42, (int)uaddr, (int)max_wake, 0);
}

static inline void* mmap(int fd, uint32_t size, int flags) {
    return (void*)syscall(21, fd, size, flags);
}

static inline int munmap(void* addr, uint32_t length) {
    return syscall(22, (int)addr, length, 0);
}

typedef struct {
    uint32_t type;
    uint32_t size;
    uint32_t inode;
    uint32_t flags;
    uint32_t created_at;
    uint32_t modified_at;
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
    return syscall(23, (int)path, (int)buf, 0);
}

static inline int statat(int dirfd, const char* path, stat_t* buf) {
    return syscall(55, dirfd, (int)path, (int)buf);
}

static inline int getdents(int fd, void* buf, uint32_t size) {
    return syscall(28, fd, (int)buf, (int)size);
}

static inline int fstatat(int dirfd, const char* name, stat_t* buf) {
    return syscall(29, dirfd, (int)name, (int)buf);
}

static inline int get_fs_info(fs_info_t* buf) {
    return syscall(24, (int)buf, 0, 0);
}

static inline int openat(int dirfd, const char* path, int flags) {
    return syscall(52, dirfd, (int)path, flags);
}

static inline int renameat(int old_dirfd, const char* oldpath, int new_dirfd, const char* newpath) {
    return syscall4(13, old_dirfd, (int)oldpath, new_dirfd, (int)newpath);
}

static inline int spawn_process(const char* path, int argc, char** argv) {
    return syscall(26, (int)path, argc, (int)argv);
}

static inline int spawn_process_resolved(const char* name, int argc, char** argv) {
    if (!name || !*name) return -1;

    if (name[0] == '/') {
        return spawn_process(name, argc, argv);
    }

    size_t n = strlen(name);
    const int has_exe = (n >= 4 && strcmp(name + (n - 4u), ".exe") == 0);

    int has_slash = 0;
    for (size_t i = 0; i < n; i++) {
        if (name[i] == '/') {
            has_slash = 1;
            break;
        }
    }

    char path[256];

    if (has_slash) {
        if (has_exe) {
            return spawn_process(name, argc, argv);
        }
        (void)snprintf(path, sizeof(path), "%s.exe", name);
        int pid = spawn_process(path, argc, argv);
        if (pid >= 0) return pid;
        return spawn_process(name, argc, argv);
    }

    if (!has_exe) {
        (void)snprintf(path, sizeof(path), "%s.exe", name);
        int pid = spawn_process(path, argc, argv);
        if (pid >= 0) return pid;
    } else {
        int pid = spawn_process(name, argc, argv);
        if (pid >= 0) return pid;
    }

    if (has_exe) {
        (void)snprintf(path, sizeof(path), "/bin/%s", name);
    } else {
        (void)snprintf(path, sizeof(path), "/bin/%s.exe", name);
    }
    int pid = spawn_process(path, argc, argv);
    if (pid >= 0) return pid;

    if (has_exe) {
        (void)snprintf(path, sizeof(path), "/bin/usr/%s", name);
    } else {
        (void)snprintf(path, sizeof(path), "/bin/usr/%s.exe", name);
    }
    return spawn_process(path, argc, argv);
}

static inline int waitpid(int pid, int* status) {
    return syscall(27, pid, (int)status, 0);
}

static inline void* map_framebuffer(void) {
    const int fd = open("/dev/fb0", 0);
    if (fd < 0) {
        return 0;
    }

    yos_fb_info_t info;
    if (ioctl(fd, YOS_FB_GET_INFO, &info) != 0) {
        (void)close(fd);
        return 0;
    }

    void* ptr = mmap(fd, info.size_bytes, MAP_SHARED);
    (void)close(fd);

    return ptr;
}

static inline int fb_acquire(void) {
    const int fd = open("/dev/fb0", 0);
    if (fd < 0) {
        return -1;
    }

    const int rc = ioctl(fd, YOS_FB_ACQUIRE, 0);
    (void)close(fd);

    return rc;
}

static inline int fb_release(void) {
    const int fd = open("/dev/fb0", 0);
    if (fd < 0) {
        return -1;
    }

    const int rc = ioctl(fd, YOS_FB_RELEASE, 0);
    (void)close(fd);

    return rc;
}

static inline int fb_present(const void* src, uint32_t src_stride, const fb_rect_t* rects, uint32_t rect_count) {
    fb_present_req_t req;
    req.src = src;
    req.src_stride = src_stride;
    req.rects = rects;
    req.rect_count = rect_count;
    return syscall(37, (int)(uintptr_t)&req, 0, 0);
}

#endif
