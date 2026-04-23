/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2026 Yula1234 */

#include <mm/dma/api.h>
#include <mm/iomem.h>
#include <mm/heap.h>

#include <kernel/locking/guards.h>
#include <kernel/smp/mb.h>

#include <lib/compiler.h>
#include <lib/string.h>

#include "virtqueue.h"

___inline uint16_t vq_mod(uint16_t x, uint16_t size) {
    return x & (size - 1u);
}

___inline int vring_need_event(uint16_t event_idx, uint16_t new_idx, uint16_t old_idx) {
    return (uint16_t)(new_idx - event_idx - 1u) < (uint16_t)(new_idx - old_idx);
}

___inline uint16_t virtqueue_read_avail_event(virtqueue_t* vq) {
    const uint16_t* avail_event_ptr =
        (const uint16_t*)(const void*)(&vq->used->ring[vq->size]);

    smp_rmb();

    return *avail_event_ptr;
}

static int virtqueue_should_notify(virtqueue_t* vq, uint16_t old_idx, uint16_t new_idx) {
    if (vq->event_idx_enabled) {
        const uint16_t avail_event = virtqueue_read_avail_event(vq);

        return vring_need_event(avail_event, new_idx, old_idx);
    }

    return (vq->used->flags & VRING_USED_F_NO_NOTIFY) == 0u;
}

___inline uint32_t virtqueue_ring_bytes(uint16_t qsz) {
    const uint32_t desc_bytes =
        (uint32_t)qsz * (uint32_t)sizeof(vring_desc_t);

    const uint32_t avail_bytes =
        (uint32_t)sizeof(vring_avail_t)
        + (uint32_t)qsz * (uint32_t)sizeof(uint16_t)
        + (uint32_t)sizeof(uint16_t);

    const uint32_t used_bytes =
        (uint32_t)sizeof(vring_used_t)
        + (uint32_t)qsz * (uint32_t)sizeof(vring_used_elem_t)
        + (uint32_t)sizeof(uint16_t);

    const uint32_t used_aligned = (used_bytes + 4095u) & ~4095u;

    uint32_t total = desc_bytes + avail_bytes;
    total = (total + 4095u) & ~4095u;
    total += used_aligned;
    total = (total + 4095u) & ~4095u;

    return total;
}

static void virtqueue_build_free_list(virtqueue_t* vq) {
    vq->free_head = 0u;

    for (uint16_t i = 0u; i < vq->size - 1u; i++) {
        vq->desc[i].next  = (uint16_t)(i + 1u);
        vq->desc[i].flags = VRING_DESC_F_NEXT;
    }

    vq->desc[vq->size - 1u].next  = 0u;
    vq->desc[vq->size - 1u].flags = 0u;

    vq->num_free = vq->size;
}

static void virtqueue_tokens_init(virtqueue_t* vq) {
    vq->token_free_head = 0u;
    vq->token_num_free  = vq->size;

    for (uint16_t i = 0u; i < vq->size; i++) {
        virtqueue_token_t* t = &vq->tokens[i];

        sem_init(&t->sem, 0);

        t->used_len = 0u;

        t->on_complete     = NULL;
        t->on_complete_ctx = NULL;
        t->auto_destroy    = 0u;

        t->owner_vq   = vq;
        t->pool_index = i;

        vq->token_next[i] = (uint16_t)(i + 1u);
    }

    if (vq->size != 0u) {
        vq->token_next[vq->size - 1u] = (uint16_t)0xFFFFu;
    }
}

static virtqueue_token_t* virtqueue_token_alloc_locked(virtqueue_t* vq) {
    if (unlikely(!vq
        || vq->token_num_free == 0u)) {
        return NULL;
    }

    const uint16_t idx = vq->token_free_head;

    if (unlikely(idx == (uint16_t)0xFFFFu
        || idx >= vq->size)) {
        return NULL;
    }

    vq->token_free_head = vq->token_next[idx];
    vq->token_next[idx] = (uint16_t)0xFFFFu;
    vq->token_num_free  = (uint16_t)(vq->token_num_free - 1u);

    virtqueue_token_t* token = &vq->tokens[idx];

    token->used_len = 0u;

    sem_reset(&token->sem, 0);

    token->on_complete     = NULL;
    token->on_complete_ctx = NULL;
    token->auto_destroy    = 0u;

    return token;
}

static void virtqueue_token_free_locked(virtqueue_t* vq, virtqueue_token_t* token) {
    if (unlikely(!vq
        || !token)) {
        return;
    }

    const uint16_t idx = token->pool_index;

    if (unlikely(idx >= vq->size)) {
        return;
    }

    token->used_len = 0u;

    sem_reset(&token->sem, 0);

    token->on_complete     = NULL;
    token->on_complete_ctx = NULL;
    token->auto_destroy    = 0u;

    vq->token_next[idx] = vq->token_free_head;
    vq->token_free_head = idx;
    vq->token_num_free  = (uint16_t)(vq->token_num_free + 1u);
}

int virtqueue_init(
    virtqueue_t* vq,
    uint16_t queue_index,
    uint16_t size,
    __iomem* notify_region,
    uint32_t notify_offset
) {
    if (unlikely(!vq
        || size == 0u)) {
        return 0;
    }

    memset(vq, 0, sizeof(*vq));

    vq->queue_index      = queue_index;
    vq->size             = size;
    vq->notify_iomem     = notify_region;
    vq->notify_iomem_off = notify_offset;

    spinlock_init(&vq->lock);

    const uint32_t ring_bytes = virtqueue_ring_bytes(size);
    uint32_t ring_phys = 0u;

    void* mem = dma_alloc_coherent((size_t)ring_bytes, &ring_phys);

    if (unlikely(!mem
        || ring_phys == 0u)) {
        return 0;
    }

    vq->ring_mem        = mem;
    vq->ring_phys       = ring_phys;
    vq->ring_alloc_size = (size_t)ring_bytes;

    uint8_t* base = (uint8_t*)mem;

    const uint32_t desc_bytes = (uint32_t)size * (uint32_t)sizeof(vring_desc_t);
    vq->desc = (vring_desc_t*)base;

    uint8_t* avail_ptr = base + desc_bytes;
    vq->avail = (vring_avail_t*)avail_ptr;

    const uint32_t avail_bytes =
        (uint32_t)sizeof(vring_avail_t)
        + (uint32_t)size * (uint32_t)sizeof(uint16_t)
        + (uint32_t)sizeof(uint16_t);

    const uint32_t used_off = (desc_bytes + avail_bytes + 4095u) & ~4095u;
    vq->used = (vring_used_t*)(base + used_off);

    vq->avail_idx     = 0u;
    vq->last_used_idx = 0u;

    virtqueue_build_free_list(vq);

    const size_t pending_bytes = (size_t)size * sizeof(virtqueue_token_t*);
    const size_t token_bytes   = (size_t)size * sizeof(virtqueue_token_t);
    const size_t next_bytes    = (size_t)size * sizeof(uint16_t);

    const size_t aux_bytes = pending_bytes + token_bytes + next_bytes;

    vq->aux_mem = kzalloc(aux_bytes);

    if (unlikely(!vq->aux_mem)) {
        virtqueue_destroy(vq);
        return 0;
    }

    uint8_t* aux = (uint8_t*)vq->aux_mem;

    vq->pending = (virtqueue_token_t**)aux;
    aux += pending_bytes;

    vq->tokens = (virtqueue_token_t*)aux;
    aux += token_bytes;

    vq->token_next = (uint16_t*)aux;

    virtqueue_tokens_init(vq);

    return 1;
}

void virtqueue_destroy(virtqueue_t* vq) {
    if (unlikely(!vq)) {
        return;
    }

    if (vq->pending) {
        for (uint16_t i = 0u; i < vq->size; i++) {
            virtqueue_token_t* t = vq->pending[i];

            if (t) {
                t->used_len = 0u;

                sem_signal(&t->sem);

                vq->pending[i] = NULL;
            }
        }

        vq->pending = NULL;
    }

    if (vq->aux_mem) {
        kfree(vq->aux_mem);
        vq->aux_mem = NULL;
    }

    vq->tokens          = NULL;
    vq->token_next      = NULL;
    vq->token_free_head = 0u;
    vq->token_num_free  = 0u;

    if (vq->ring_mem) {
        dma_free_coherent(vq->ring_mem, vq->ring_alloc_size, vq->ring_phys);

        vq->ring_mem        = NULL;
        vq->ring_phys       = 0u;
        vq->ring_alloc_size = 0u;
    }

    vq->desc  = NULL;
    vq->avail = NULL;
    vq->used  = NULL;

    vq->notify_iomem     = NULL;
    vq->notify_iomem_off = 0u;
}

static int virtqueue_alloc_desc_chain(virtqueue_t* vq, uint16_t count, uint16_t* out_head) {
    if (unlikely(vq->num_free < count)) {
        return 0;
    }

    const uint16_t head = vq->free_head;
    uint16_t cur = head;

    for (uint16_t i = 0u; i < count; i++) {
        const uint16_t next = vq->desc[cur].next;

        if (i == count - 1u) {
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
    uint16_t count = 0u;

    for (;;) {
        const uint16_t flags = vq->desc[cur].flags;
        const uint16_t next  = vq->desc[cur].next;

        vq->desc[cur].flags = 0u;
        count++;

        if ((flags & VRING_DESC_F_NEXT) == 0u) {
            break;
        }

        cur = next;
    }

    vq->desc[cur].next  = vq->free_head;
    vq->desc[cur].flags |= VRING_DESC_F_NEXT;

    vq->free_head = head;
    vq->num_free  = (uint16_t)(vq->num_free + count);
}

___inline int __virtqueue_do_submit(
    virtqueue_t* vq,
    const uint64_t* addrs,
    const uint32_t* lens,
    const uint16_t* flags,
    uint16_t count,
    void (*on_complete)(virtqueue_token_t*, void*),
    void* on_complete_ctx,
    uint8_t auto_destroy,
    uint16_t* out_head,
    virtqueue_token_t** out_token
) {
    if (unlikely(!vq
        || !addrs
        || !lens
        || !flags
        || count == 0u
        || count > vq->size)) {
        return 0;
    }

    guard_spinlock_safe(&vq->lock);

    virtqueue_token_t* token = virtqueue_token_alloc_locked(vq);

    if (unlikely(!token)) {
        return 0;
    }

    token->on_complete     = on_complete;
    token->on_complete_ctx = on_complete_ctx;
    token->auto_destroy    = auto_destroy;

    uint16_t head = 0u;

    if (unlikely(!virtqueue_alloc_desc_chain(vq, count, &head))) {
        goto err_free_token;
    }

    if (unlikely(vq->pending && vq->pending[head])) {
        goto err_free_desc;
    }

    vq->pending[head] = token;

    uint16_t cur = head;

    for (uint16_t i = 0u; i < count; i++) {
        const uint16_t next = vq->desc[cur].next;

        vq->desc[cur].addr = addrs[i];
        vq->desc[cur].len  = lens[i];

        uint16_t f = flags[i];

        if (i + 1u < count) {
            vq->desc[cur].next = next;
            f |= VRING_DESC_F_NEXT;
            vq->desc[cur].flags = f;

            cur = next;
        } else {
            vq->desc[cur].next = 0u;
            f &= (uint16_t)~VRING_DESC_F_NEXT;
            vq->desc[cur].flags = f;
        }
    }

    const uint16_t avail_slot = vq_mod(vq->avail_idx, vq->size);

    vq->avail->ring[avail_slot] = head;

    smp_wmb();

    const uint16_t old_idx = vq->avail_idx;
    const uint16_t new_idx = old_idx + 1u;

    vq->avail_idx  = new_idx;
    vq->avail->idx = new_idx;

    smp_mb();

    if (vq->notify_iomem && virtqueue_should_notify(vq, old_idx, new_idx)) {
        iowrite16(vq->notify_iomem, vq->notify_iomem_off, vq->queue_index);
    }

    if (out_head) {
        *out_head = head;
    }

    if (out_token) {
        *out_token = token;
    }

    return 1;

err_free_desc:
    virtqueue_free_desc_chain(vq, head);

err_free_token:
    virtqueue_token_free_locked(vq, token);

    return 0;
}

int virtqueue_submit(
    virtqueue_t* vq,
    const uint64_t* addrs,
    const uint32_t* lens,
    const uint16_t* flags,
    uint16_t count,
    uint16_t* out_head,
    virtqueue_token_t** out_token
) {
    return __virtqueue_do_submit(
        vq, addrs, lens, flags, count,
        NULL, NULL, 0u, out_head, out_token
    );
}

int virtqueue_submit_cb(
    virtqueue_t* vq,
    const uint64_t* addrs,
    const uint32_t* lens,
    const uint16_t* flags,
    uint16_t count,
    void (*on_complete)(virtqueue_token_t*, void*),
    void* on_complete_ctx,
    uint8_t auto_destroy
) {
    return __virtqueue_do_submit(
        vq, addrs, lens, flags, count,
        on_complete, on_complete_ctx, auto_destroy,
        NULL, NULL
    );
}

uint32_t virtqueue_token_wait(virtqueue_token_t* token) {
    if (unlikely(!token)) {
        return 0u;
    }

    sem_wait(&token->sem);

    return token->used_len;
}

uint32_t virtqueue_token_wait_timeout(virtqueue_token_t* token, uint32_t deadline_tick) {
    if (unlikely(!token)) {
        return 0u;
    }

    if (unlikely(!sem_wait_timeout(&token->sem, deadline_tick))) {
        return 0u;
    }

    return token->used_len;
}

void virtqueue_token_destroy(virtqueue_token_t* token) {
    if (unlikely(!token)) {
        return;
    }

    virtqueue_t* vq = token->owner_vq;

    if (unlikely(!vq)) {
        return;
    }

    guard_spinlock_safe(&vq->lock);

    virtqueue_token_free_locked(vq, token);
}

void virtqueue_handle_irq(virtqueue_t* vq) {
    if (unlikely(!vq)) {
        return;
    }

    typedef struct {
        virtqueue_token_t* token_;
        void (*on_complete_)(virtqueue_token_t*, void*);
        void*   on_complete_ctx_;
        uint8_t auto_destroy_;
    } CompletionEntry;

    CompletionEntry completions[32];
    uint32_t completion_count = 0u;

    {
        guard_spinlock_safe(&vq->lock);

        for (;;) {
            const uint16_t used_idx = vq->used->idx;

            smp_rmb();

            while (vq->last_used_idx != used_idx) {
                const uint16_t slot = vq_mod(vq->last_used_idx, vq->size);
                const vring_used_elem_t e = vq->used->ring[slot];

                const uint16_t head = (uint16_t)e.id;

                if (likely(head < vq->size)) {
                    virtqueue_token_t* token = vq->pending ? vq->pending[head] : NULL;

                    if (likely(token)) {
                        token->used_len = e.len;
                        vq->pending[head] = NULL;

                        virtqueue_free_desc_chain(vq, head);

                        sem_signal(&token->sem);

                        if (token->on_complete && completion_count < 32u) {
                            completions[completion_count].token_           = token;
                            completions[completion_count].on_complete_     = token->on_complete;
                            completions[completion_count].on_complete_ctx_ = token->on_complete_ctx;
                            completions[completion_count].auto_destroy_    = token->auto_destroy;

                            completion_count++;
                        }
                    } else {
                        virtqueue_free_desc_chain(vq, head);
                    }
                }

                vq->last_used_idx++;
            }

            if (vq->event_idx_enabled) {
                uint16_t* used_event_ptr = (uint16_t*)(
                    (uintptr_t)vq->avail
                    + sizeof(vring_avail_t)
                    + vq->size * sizeof(uint16_t)
                );

                *used_event_ptr = vq->last_used_idx;

                smp_mb();

                if (vq->used->idx != vq->last_used_idx) {
                    continue;
                }
            }

            break;
        }
    }

    for (uint32_t i = 0; i < completion_count; i++) {
        CompletionEntry* c = &completions[i];

        c->on_complete_(c->token_, c->on_complete_ctx_);

        if (c->auto_destroy_) {
            virtqueue_token_destroy(c->token_);
        }
    }
}

void virtqueue_set_event_idx(virtqueue_t* vq, int enabled) {
    if (unlikely(!vq)) {
        return;
    }

    guard_spinlock_safe(&vq->lock);

    vq->event_idx_enabled = enabled ? 1u : 0u;
}