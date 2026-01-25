// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 Yula1234

#ifndef YOS_COMP_IPC_H
#define YOS_COMP_IPC_H

#include <stdint.h>

int write(int fd, const void* buf, uint32_t size);

#define COMP_IPC_MAGIC   0x43495043u /* 'CPIC' */
#define COMP_IPC_VERSION 1u

#define COMP_IPC_MAX_PAYLOAD 512u

typedef enum {
    COMP_IPC_MSG_HELLO      = 1,
    COMP_IPC_MSG_ATTACH_SHM = 2,
    COMP_IPC_MSG_ATTACH_SHM_NAME = 5,
    COMP_IPC_MSG_COMMIT     = 3,
    COMP_IPC_MSG_INPUT      = 4,
    COMP_IPC_MSG_DESTROY_SURFACE = 6,
    COMP_IPC_MSG_ACK        = 7,
    COMP_IPC_MSG_ERROR      = 8,
    COMP_IPC_MSG_WM_EVENT   = 9,
    COMP_IPC_MSG_WM_CMD     = 10,
    COMP_IPC_MSG_INPUT_RING_NAME = 11,
    COMP_IPC_MSG_INPUT_RING_ACK  = 12,
} comp_ipc_msg_type_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint16_t type;
    uint32_t len;
    uint32_t seq;
} comp_ipc_hdr_t;

typedef struct __attribute__((packed)) {
    uint32_t client_pid;
    uint32_t reserved;
} comp_ipc_hello_t;

typedef struct __attribute__((packed)) {
    uint32_t surface_id;
    uint32_t shm_fd;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t format;
} comp_ipc_attach_shm_t;

typedef struct __attribute__((packed)) {
    uint32_t surface_id;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t format;
    uint32_t size_bytes;
    char shm_name[32];
} comp_ipc_attach_shm_name_t;

typedef struct __attribute__((packed)) {
    uint32_t surface_id;
    int32_t  x;
    int32_t  y;
    uint32_t flags;
} comp_ipc_commit_t;

#define COMP_IPC_COMMIT_FLAG_RAISE 1u
#define COMP_IPC_COMMIT_FLAG_ACK   2u

typedef struct __attribute__((packed)) {
    uint32_t surface_id;
    uint32_t flags;
} comp_ipc_destroy_surface_t;

typedef struct __attribute__((packed)) {
    uint16_t req_type;
    uint16_t reserved;
    uint32_t surface_id;
    uint32_t flags;
} comp_ipc_ack_t;

typedef struct __attribute__((packed)) {
    uint16_t req_type;
    uint16_t code;
    uint32_t surface_id;
    uint32_t detail;
} comp_ipc_error_t;

#define COMP_IPC_ERR_INVALID    1u
#define COMP_IPC_ERR_NO_SURFACE 2u
#define COMP_IPC_ERR_SHM_OPEN   3u
#define COMP_IPC_ERR_SHM_MAP    4u

#define COMP_WM_EVENT_MAP   1u
#define COMP_WM_EVENT_UNMAP 2u
#define COMP_WM_EVENT_CLICK 3u
#define COMP_WM_EVENT_COMMIT 4u
#define COMP_WM_EVENT_KEY 5u
#define COMP_WM_EVENT_POINTER 6u

#define COMP_WM_CLIENT_NONE 0xFFFFFFFFu

#define COMP_WM_EVENT_FLAG_REPLAY 0x00000001u
#define COMP_WM_EVENT_FLAG_BACKGROUND 0x00000002u

typedef struct __attribute__((packed)) {
    uint32_t kind;
    uint32_t client_id;
    uint32_t surface_id;
    int32_t  sx;
    int32_t  sy;
    uint32_t sw;
    uint32_t sh;
    int32_t  px;
    int32_t  py;
    uint32_t buttons;
    uint32_t keycode;
    uint32_t key_state;
    uint32_t flags;
} comp_ipc_wm_event_t;

#define COMP_WM_CMD_FOCUS 1u
#define COMP_WM_CMD_RAISE 2u
#define COMP_WM_CMD_MOVE  3u
#define COMP_WM_CMD_CLOSE 4u
#define COMP_WM_CMD_POINTER_GRAB 5u
#define COMP_WM_CMD_RESIZE 6u
#define COMP_WM_CMD_PREVIEW_RECT 7u
#define COMP_WM_CMD_PREVIEW_CLEAR 8u
#define COMP_WM_CMD_EXIT 9u

typedef struct __attribute__((packed)) {
    uint32_t kind;
    uint32_t client_id;
    uint32_t surface_id;
    int32_t  x;
    int32_t  y;
    uint32_t flags;
} comp_ipc_wm_cmd_t;

typedef struct __attribute__((packed)) {
    uint32_t surface_id;
    uint32_t kind;
    int32_t  x;
    int32_t  y;
    uint32_t buttons;
    uint32_t keycode;
    uint32_t key_state;
} comp_ipc_input_t;

#define COMP_IPC_INPUT_MOUSE 1u
#define COMP_IPC_INPUT_KEY   2u
#define COMP_IPC_INPUT_RESIZE 3u


#define COMP_INPUT_RING_MAGIC 0x49525043u
#define COMP_INPUT_RING_VERSION 1u

#define COMP_INPUT_RING_CAP 2048u
#define COMP_INPUT_RING_MASK (COMP_INPUT_RING_CAP - 1u)

#define COMP_INPUT_RING_FLAG_READY  1u
#define COMP_INPUT_RING_FLAG_WAIT_W 2u
#define COMP_INPUT_RING_FLAG_WAIT_R 4u

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t cap;
    uint32_t mask;
    volatile uint32_t r;
    volatile uint32_t w;
    volatile uint32_t dropped;
    volatile uint32_t flags;
    comp_ipc_input_t events[COMP_INPUT_RING_CAP];
} comp_input_ring_t;

typedef struct __attribute__((packed)) {
    uint32_t size_bytes;
    uint32_t cap;
    uint32_t reserved;
    char shm_name[32];
} comp_ipc_input_ring_name_t;

static inline int comp_ipc_write_full(int fd, const void* buf, uint32_t size) {
    const uint8_t* p = (const uint8_t*)buf;
    uint32_t done = 0;
    while (done < size) {
        int r = write(fd, (const void*)(p + done), size - done);
        if (r <= 0) return -1;
        done += (uint32_t)r;
    }
    return (int)done;
}

static inline int comp_ipc_send(int fd, uint16_t type, uint32_t seq, const void* payload, uint32_t payload_len) {
    if (payload_len > COMP_IPC_MAX_PAYLOAD) return -1;

    comp_ipc_hdr_t h;
    h.magic = COMP_IPC_MAGIC;
    h.version = (uint16_t)COMP_IPC_VERSION;
    h.type = type;
    h.len = payload_len;
    h.seq = seq;

    if (comp_ipc_write_full(fd, &h, (uint32_t)sizeof(h)) < 0) return -1;
    if (payload_len == 0) return 0;
    return comp_ipc_write_full(fd, payload, payload_len) < 0 ? -1 : 0;
}

#endif
