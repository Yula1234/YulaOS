/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2026 Yula1234 */

#ifndef LIB_RINGBUF_H
#define LIB_RINGBUF_H

#include <lib/compiler.h>
#include <lib/string.h>

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {

#define likely kernel::likely
#define unlikely kernel::unlikely

#endif

/*
 * Byte-oriented Circular Buffer.
 *
 * Designed to be protected by an external lock.
 * Stores its own count_ to distinguish between empty and full states,
 * allowing full utilization of the underlying buffer capacity.
 */
typedef struct ringbuf {
    uint8_t* buffer_;
    
    size_t   capacity_;
    size_t   count_;
    
    size_t   head_;
    size_t   tail_;
} ringbuf_t;

___inline void ringbuf_init(ringbuf_t* rb, uint8_t* buffer, size_t capacity) {
    if (unlikely(!rb
        || !buffer
        || capacity == 0u))
        return;

    memset(buffer, 0, capacity);

    rb->buffer_   = buffer;
    rb->capacity_ = capacity;
    
    rb->count_    = 0u;
    rb->head_     = 0u;
    rb->tail_     = 0u;
}

___inline void ringbuf_clear(ringbuf_t* rb) {
    if (unlikely(!rb))
        return;

    rb->count_ = 0u;
    rb->head_  = 0u;
    rb->tail_  = 0u;
}

___inline size_t ringbuf_size(const ringbuf_t* rb) {
    if (unlikely(!rb))
        return 0u;

    return rb->count_;
}

___inline size_t ringbuf_capacity(const ringbuf_t* rb) {
    if (unlikely(!rb))
        return 0u;

    return rb->capacity_;
}

___inline size_t ringbuf_free_space(const ringbuf_t* rb) {
    if (unlikely(!rb))
        return 0u;

    return rb->capacity_ - rb->count_;
}

___inline int ringbuf_is_empty(const ringbuf_t* rb) {
    if (unlikely(!rb))
        return 1;

    return rb->count_ == 0u;
}

___inline int ringbuf_is_full(const ringbuf_t* rb) {
    if (unlikely(!rb))
        return 1;

    return rb->count_ == rb->capacity_;
}

/*
 * Push a single byte into the ring buffer.
 * Returns 1 on success, 0 if the buffer is full.
 */
___inline size_t ringbuf_push(ringbuf_t* rb, uint8_t data) {
    if (unlikely(!rb
        || rb->count_ >= rb->capacity_))
        return 0u;

    rb->buffer_[rb->head_] = data;
    
    rb->head_++;
    
    if (unlikely(rb->head_ >= rb->capacity_))
        rb->head_ = 0u;
    
    rb->count_++;

    return 1u;
}

/*
 * Pop a single byte from the ring buffer.
 * Returns 1 on success, 0 if the buffer is empty.
 */
___inline size_t ringbuf_pop(ringbuf_t* rb, uint8_t* out_data) {
    if (unlikely(!rb
        || !out_data
        || rb->count_ == 0u))
        return 0u;

    *out_data = rb->buffer_[rb->tail_];
    
    rb->tail_++;
    
    if (unlikely(rb->tail_ >= rb->capacity_))
        rb->tail_ = 0u;
    
    rb->count_--;

    return 1u;
}

/*
 * Write a block of data into the ring buffer.
 * Returns the number of bytes actually written, may be less than requested 
 * if there is insufficient free space.
 */
___inline size_t ringbuf_write(ringbuf_t* rb, const uint8_t* data, size_t size) {
    if (unlikely(!rb
        || !data
        || size == 0u))
        return 0u;

    const size_t space = rb->capacity_ - rb->count_;

    if (size > space)
        size = space;

    if (size == 0u)
        return 0u;

    const size_t contig = rb->capacity_ - rb->head_;

    if (size <= contig) {
        memcpy(rb->buffer_ + rb->head_, data, size);
        
        rb->head_ += size;
        
        if (unlikely(rb->head_ == rb->capacity_))
            rb->head_ = 0u;

    } else {
        memcpy(rb->buffer_ + rb->head_, data, contig);
        memcpy(rb->buffer_, data + contig, size - contig);
        
        rb->head_ = size - contig;
    }

    rb->count_ += size;

    return size;
}

/*
 * Read a block of data from the ring buffer.
 * Returns the number of bytes actually read.
 */
___inline size_t ringbuf_read(ringbuf_t* rb, uint8_t* out_data, size_t size) {
    if (unlikely(!rb
        || !out_data
        || size == 0u))
        return 0u;

    if (size > rb->count_)
        size = rb->count_;

    if (size == 0u)
        return 0u;

    const size_t contig = rb->capacity_ - rb->tail_;

    if (size <= contig) {
        memcpy(out_data, rb->buffer_ + rb->tail_, size);
        
        rb->tail_ += size;
        
        if (unlikely(rb->tail_ == rb->capacity_))
            rb->tail_ = 0u;

    } else {
        memcpy(out_data, rb->buffer_ + rb->tail_, contig);
        memcpy(out_data + contig, rb->buffer_, size - contig);
        
        rb->tail_ = size - contig;
    }

    rb->count_ -= size;

    return size;
}

/*
 * Peek at the largest contiguous chunk of readable data without copying.
 * Populates out_ptr with the start address, and returns the chunk length.
 */
___inline size_t ringbuf_peek_contiguous(const ringbuf_t* rb, const uint8_t** out_ptr) {
    if (unlikely(!rb
        || !out_ptr))
        return 0u;

    if (unlikely(rb->count_ == 0u)) {
        *out_ptr = NULL;

        return 0u;
    }

    *out_ptr = &rb->buffer_[rb->tail_];

    const size_t contig = rb->capacity_ - rb->tail_;

    if (rb->count_ < contig)
        return rb->count_;

    return contig;
}

/*
 * Discard up to 'size' bytes from the read end of the buffer.
 * Used in conjunction with ringbuf_peek_contiguous().
 */
___inline void ringbuf_consume(ringbuf_t* rb, size_t size) {
    if (unlikely(!rb
        || size == 0u))
        return;

    if (unlikely(size > rb->count_))
        size = rb->count_;

    rb->tail_ += size;
    
    if (unlikely(rb->tail_ >= rb->capacity_))
        rb->tail_ -= rb->capacity_;

    rb->count_ -= size;
}

#ifdef __cplusplus
}
#endif

#endif /* LIB_RINGBUF_H */