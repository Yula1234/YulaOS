// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 Yula1234

#ifndef DRIVERS_VIRTQUEUE_H
#define DRIVERS_VIRTQUEUE_H

#include <hal/lock.h>

#include <stdint.h>

typedef struct __attribute__((packed)) {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} vring_desc_t;

#define VRING_DESC_F_NEXT  1u
#define VRING_DESC_F_WRITE 2u

typedef struct __attribute__((packed)) {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[];
} vring_avail_t;

typedef struct __attribute__((packed)) {
    uint32_t id;
    uint32_t len;
} vring_used_elem_t;

typedef struct __attribute__((packed)) {
    uint16_t flags;
    uint16_t idx;
    vring_used_elem_t ring[];
} vring_used_t;

typedef struct virtqueue_token {
    semaphore_t sem;
    uint32_t used_len;
} virtqueue_token_t;

typedef struct virtqueue {
    uint16_t queue_index;
    uint16_t size;

    vring_desc_t* desc;
    vring_avail_t* avail;
    vring_used_t* used;

    volatile uint16_t* notify_addr;

    void* ring_mem;
    uint32_t ring_order;

    uint16_t free_head;
    uint16_t num_free;

    uint16_t avail_idx;
    uint16_t last_used_idx;

    virtqueue_token_t** pending;

    spinlock_t lock;
} virtqueue_t;

int virtqueue_init(virtqueue_t* vq, uint16_t queue_index, uint16_t size, volatile uint16_t* notify_addr);
void virtqueue_destroy(virtqueue_t* vq);

int virtqueue_submit(virtqueue_t* vq,
                     const uint64_t* addrs,
                     const uint32_t* lens,
                     const uint16_t* flags,
                     uint16_t count,
                     uint16_t* out_head,
                     virtqueue_token_t** out_token);

uint32_t virtqueue_token_wait(virtqueue_token_t* token);
void virtqueue_token_destroy(virtqueue_token_t* token);

void virtqueue_handle_irq(virtqueue_t* vq);

#endif
