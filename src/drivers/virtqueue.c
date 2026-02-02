// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 Yula1234

#include <arch/i386/paging.h>

#include <mm/pmm.h>
#include <mm/heap.h>

#include <lib/string.h>

#include "virtqueue.h"

static inline uint16_t vq_mod(uint16_t x, uint16_t size) {
    return (uint16_t)(x % size);
}

static uint32_t virtqueue_ring_bytes(uint16_t qsz) {
    uint32_t desc_bytes = (uint32_t)qsz * (uint32_t)sizeof(vring_desc_t);

    uint32_t avail_bytes = (uint32_t)sizeof(vring_avail_t) + (uint32_t)qsz * (uint32_t)sizeof(uint16_t) + (uint32_t)sizeof(uint16_t);

    uint32_t used_bytes = (uint32_t)sizeof(vring_used_t) + (uint32_t)qsz * (uint32_t)sizeof(vring_used_elem_t) + (uint32_t)sizeof(uint16_t);

    uint32_t used_aligned = (used_bytes + 4095u) & ~4095u;
    uint32_t total = desc_bytes + avail_bytes;
    total = (total + 4095u) & ~4095u;
    total += used_aligned;
    total = (total + 4095u) & ~4095u;
    return total;
}

static uint32_t virtqueue_ring_order(uint32_t bytes) {
    uint32_t pages = (bytes + 4095u) >> 12;

    uint32_t order = 0;
    uint32_t pow2 = 1;
    while (pow2 < pages && order < 31) {
        pow2 <<= 1;
        order++;
    }
    return order;
}

static void virtqueue_build_free_list(virtqueue_t* vq) {
    vq->free_head = 0;
    for (uint16_t i = 0; i < vq->size - 1; i++) {
        vq->desc[i].next = (uint16_t)(i + 1);
        vq->desc[i].flags = VRING_DESC_F_NEXT;
    }
    vq->desc[vq->size - 1].next = 0;
    vq->desc[vq->size - 1].flags = 0;
    vq->num_free = vq->size;
}

int virtqueue_init(virtqueue_t* vq, uint16_t queue_index, uint16_t size, volatile uint16_t* notify_addr) {
    if (!vq || size == 0) return 0;

    memset(vq, 0, sizeof(*vq));

    vq->queue_index = queue_index;
    vq->size = size;
    vq->notify_addr = notify_addr;

    spinlock_init(&vq->lock);

    uint32_t ring_bytes = virtqueue_ring_bytes(size);
    uint32_t order = virtqueue_ring_order(ring_bytes);

    void* mem = pmm_alloc_pages(order);
    if (!mem) return 0;

    memset(mem, 0, (size_t)PAGE_SIZE << order);

    vq->ring_mem = mem;
    vq->ring_order = order;

    uint8_t* base = (uint8_t*)mem;

    uint32_t desc_bytes = (uint32_t)size * (uint32_t)sizeof(vring_desc_t);
    vq->desc = (vring_desc_t*)base;

    uint8_t* avail_ptr = base + desc_bytes;
    vq->avail = (vring_avail_t*)avail_ptr;

    uint32_t avail_bytes = (uint32_t)sizeof(vring_avail_t) + (uint32_t)size * (uint32_t)sizeof(uint16_t) + (uint32_t)sizeof(uint16_t);
    uint32_t used_off = (desc_bytes + avail_bytes + 4095u) & ~4095u;

    vq->used = (vring_used_t*)(base + used_off);

    vq->avail_idx = 0;
    vq->last_used_idx = 0;

    virtqueue_build_free_list(vq);

    vq->pending = (virtqueue_token_t**)kzalloc((size_t)size * sizeof(virtqueue_token_t*));
    if (!vq->pending) {
        virtqueue_destroy(vq);
        return 0;
    }

    return 1;
}

void virtqueue_destroy(virtqueue_t* vq) {
    if (!vq) return;

    if (vq->pending) {
        for (uint16_t i = 0; i < vq->size; i++) {
            virtqueue_token_t* t = vq->pending[i];
            if (t) {
                t->used_len = 0;
                sem_signal(&t->sem);
                vq->pending[i] = 0;
            }
        }
        kfree(vq->pending);
        vq->pending = 0;
    }

    if (vq->ring_mem) {
        pmm_free_pages(vq->ring_mem, vq->ring_order);
        vq->ring_mem = 0;
    }

    vq->desc = 0;
    vq->avail = 0;
    vq->used = 0;
    vq->notify_addr = 0;
}

static int virtqueue_alloc_desc_chain(virtqueue_t* vq, uint16_t count, uint16_t* out_head) {
    if (vq->num_free < count) return 0;

    uint16_t head = vq->free_head;
    uint16_t cur = head;

    for (uint16_t i = 0; i < count; i++) {
        uint16_t next = vq->desc[cur].next;
        if (i == count - 1) {
            vq->free_head = next;
        }
        cur = next;
    }

    vq->num_free = (uint16_t)(vq->num_free - count);
    *out_head = head;
    return 1;
}

static void virtqueue_free_desc_chain(virtqueue_t* vq, uint16_t head) {
    uint16_t cur = head;
    uint16_t count = 0;

    while (1) {
        uint16_t flags = vq->desc[cur].flags;
        uint16_t next = vq->desc[cur].next;

        vq->desc[cur].flags = 0;

        count++;
        if (!(flags & VRING_DESC_F_NEXT)) {
            break;
        }
        cur = next;
    }

    vq->desc[cur].next = vq->free_head;
    vq->desc[cur].flags |= VRING_DESC_F_NEXT;
    vq->free_head = head;
    vq->num_free = (uint16_t)(vq->num_free + count);
}

int virtqueue_submit(virtqueue_t* vq,
                     const uint64_t* addrs,
                     const uint32_t* lens,
                     const uint16_t* flags,
                     uint16_t count,
                     uint16_t* out_head,
                     virtqueue_token_t** out_token) {
    if (!vq || !addrs || !lens || !flags || count == 0 || count > vq->size) return 0;

    virtqueue_token_t* token = (virtqueue_token_t*)kzalloc(sizeof(virtqueue_token_t));
    if (!token) return 0;
    sem_init(&token->sem, 0);

    uint32_t iflags = spinlock_acquire_safe(&vq->lock);

    uint16_t head;
    if (!virtqueue_alloc_desc_chain(vq, count, &head)) {
        spinlock_release_safe(&vq->lock, iflags);
        virtqueue_token_destroy(token);
        return 0;
    }

    if (vq->pending && vq->pending[head]) {
        virtqueue_free_desc_chain(vq, head);
        spinlock_release_safe(&vq->lock, iflags);
        virtqueue_token_destroy(token);
        return 0;
    }

    vq->pending[head] = token;

    uint16_t cur = head;
    for (uint16_t i = 0; i < count; i++) {
        uint16_t next = vq->desc[cur].next;

        vq->desc[cur].addr = addrs[i];
        vq->desc[cur].len = lens[i];
        uint16_t f = flags[i];

        if (i + 1u < count) {
            vq->desc[cur].next = next;
            f |= VRING_DESC_F_NEXT;
            vq->desc[cur].flags = f;
            cur = next;
        } else {
            vq->desc[cur].next = 0;
            f &= (uint16_t)~VRING_DESC_F_NEXT;
            vq->desc[cur].flags = f;
        }
    }

    uint16_t avail_slot = vq_mod(vq->avail_idx, vq->size);
    vq->avail->ring[avail_slot] = head;

    __sync_synchronize();
    vq->avail_idx++;
    vq->avail->idx = vq->avail_idx;

    __sync_synchronize();

    if (vq->notify_addr) {
        *vq->notify_addr = vq->queue_index;
    }

    spinlock_release_safe(&vq->lock, iflags);

    if (out_head) *out_head = head;
    if (out_token) *out_token = token;
    return 1;
}

uint32_t virtqueue_token_wait(virtqueue_token_t* token) {
    if (!token) return 0;
    sem_wait(&token->sem);
    return token->used_len;
}

void virtqueue_token_destroy(virtqueue_token_t* token) {
    if (!token) return;
    kfree(token);
}

void virtqueue_handle_irq(virtqueue_t* vq) {
    if (!vq) return;

    uint32_t iflags = spinlock_acquire_safe(&vq->lock);

    __sync_synchronize();
    uint16_t used_idx = vq->used->idx;

    while (vq->last_used_idx != used_idx) {
        uint16_t slot = vq_mod(vq->last_used_idx, vq->size);
        vring_used_elem_t e = vq->used->ring[slot];

        uint16_t head = (uint16_t)e.id;
        if (head < vq->size) {
            virtqueue_token_t* token = vq->pending ? vq->pending[head] : 0;
            if (token) {
                token->used_len = e.len;
                vq->pending[head] = 0;
                virtqueue_free_desc_chain(vq, head);
                sem_signal(&token->sem);
            } else {
                virtqueue_free_desc_chain(vq, head);
            }
        }

        vq->last_used_idx++;
    }

    spinlock_release_safe(&vq->lock, iflags);
}
