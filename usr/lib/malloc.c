/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2026 Yula1234 */

#include <yula.h>

#ifndef likely
#if defined(__GNUC__) || defined(__clang__)
#define likely(x)   __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)
#else
#define likely(x)   (x)
#define unlikely(x) (x)
#endif
#endif

#define MALLOC_ALIGNMENT       8u
#define MALLOC_MIN_SIZE        16u
#define MALLOC_SYS_ALLOC_MIN   65536u
#define MALLOC_TRIM_THRESHOLD  (256u * 1024u)

#define CHUNK_IN_USE_PREV      1u
#define CHUNK_IS_FREE          2u
#define CHUNK_FLAG_MASK        3u

#define NUM_BINS               64u

typedef struct MallocChunk {
    uint32_t prev_size_;
    uint32_t size_;

    struct MallocChunk* fd_;
    struct MallocChunk* bk_;
} MallocChunk;

typedef struct MallocState {
    MallocChunk bins_[NUM_BINS];

    MallocChunk* top_chunk_;
    uint32_t top_size_;

    int initialized_;
} MallocState;

static pthread_mutex_t g_malloc_lock = PTHREAD_MUTEX_INITIALIZER;
static MallocState g_state;

static inline void lock_heap(void) {
    (void)pthread_mutex_lock(&g_malloc_lock);
}

static inline void unlock_heap(void) {
    (void)pthread_mutex_unlock(&g_malloc_lock);
}

static inline uint32_t chunk_size(const MallocChunk* c) {
    return c->size_ & ~CHUNK_FLAG_MASK;
}

static inline int chunk_prev_in_use(const MallocChunk* c) {
    return (c->size_ & CHUNK_IN_USE_PREV) != 0u;
}

static inline int chunk_is_free(const MallocChunk* c) {
    return (c->size_ & CHUNK_IS_FREE) != 0u;
}

static inline MallocChunk* chunk_next(const MallocChunk* c) {
    return (MallocChunk*)((char*)c + chunk_size(c));
}

static inline void chunk_set_size_and_flags(MallocChunk* c, uint32_t size, uint32_t flags) {
    c->size_ = size | flags;
}

static inline void* chunk_to_mem(MallocChunk* c) {
    return (void*)((char*)c + 8u);
}

static inline MallocChunk* mem_to_chunk(void* mem) {
    return (MallocChunk*)((char*)mem - 8u);
}

static inline uint32_t compute_bin_index(uint32_t size) {
    if (size <= 256u) {
        return (size >> 3u) - 2u;
    }

    uint32_t idx = 31u;
    uint32_t remaining = size >> 9u;

    while (remaining > 0u && idx < NUM_BINS - 1u) {
        remaining >>= 1u;
        idx++;
    }

    return idx;
}

static void bin_insert(MallocChunk* chunk, uint32_t size) {
    const uint32_t idx = compute_bin_index(size);
    MallocChunk* head = &g_state.bins_[idx];

    MallocChunk* fwd = head->fd_;

    chunk->fd_ = fwd;
    chunk->bk_ = head;

    fwd->bk_ = chunk;
    head->fd_ = chunk;
}

static void bin_remove(MallocChunk* chunk) {
    MallocChunk* fwd = chunk->fd_;
    MallocChunk* bck = chunk->bk_;

    bck->fd_ = fwd;
    fwd->bk_ = bck;
}

static void ensure_inited(void) {
    if (likely(g_state.initialized_)) {
        return;
    }

    for (uint32_t i = 0; i < NUM_BINS; i++) {
        g_state.bins_[i].fd_ = &g_state.bins_[i];
        g_state.bins_[i].bk_ = &g_state.bins_[i];
        g_state.bins_[i].size_ = 0u;
    }

    g_state.top_chunk_ = NULL;
    g_state.top_size_ = 0u;

    g_state.initialized_ = 1;
}

static void free_internal(void* mem);

static int extend_heap(uint32_t min_size) {
    uint32_t request = min_size;

    if (request < MALLOC_SYS_ALLOC_MIN) {
        request = MALLOC_SYS_ALLOC_MIN;
    }

    request = (request + 4095u) & ~4095u;

    void* p = sbrk((int)request);

    if (unlikely((int)(uintptr_t)p == -1)) {
        return 0;
    }

    if (g_state.top_chunk_ != NULL
        && (char*)p == (char*)g_state.top_chunk_ + g_state.top_size_) {

        g_state.top_size_ += request;
    } else {
        if (g_state.top_chunk_ != NULL && g_state.top_size_ > 0u) {
            const uint32_t old_size = g_state.top_size_;

            if (old_size >= MALLOC_MIN_SIZE + 8u) {
                MallocChunk* dummy = (MallocChunk*)((char*)g_state.top_chunk_ + old_size - 8u);
                chunk_set_size_and_flags(dummy, 8u, CHUNK_IN_USE_PREV);

                MallocChunk* old_top = g_state.top_chunk_;
                chunk_set_size_and_flags(old_top, old_size - 8u, CHUNK_IN_USE_PREV);

                free_internal(chunk_to_mem(old_top));
            } else {
                MallocChunk* dummy = g_state.top_chunk_;
                chunk_set_size_and_flags(dummy, old_size, CHUNK_IN_USE_PREV);
            }
        }

        g_state.top_chunk_ = (MallocChunk*)p;
        g_state.top_size_ = request;
    }

    return 1;
}

static void split_and_insert(MallocChunk* c, uint32_t total_size, uint32_t request_size) {
    const uint32_t rem = total_size - request_size;

    if (rem >= MALLOC_MIN_SIZE) {
        chunk_set_size_and_flags(c, request_size, c->size_ & CHUNK_FLAG_MASK);

        MallocChunk* remainder = chunk_next(c);
        chunk_set_size_and_flags(remainder, rem, CHUNK_IN_USE_PREV | CHUNK_IS_FREE);

        MallocChunk* next_after = chunk_next(remainder);

        if (next_after == g_state.top_chunk_) {
            g_state.top_chunk_ = remainder;
            g_state.top_size_ += rem;
        } else {
            next_after->size_ &= ~CHUNK_IN_USE_PREV;
            next_after->prev_size_ = rem;

            bin_insert(remainder, rem);
        }
    } else {
        MallocChunk* next = chunk_next(c);

        if (next != g_state.top_chunk_) {
            next->size_ |= CHUNK_IN_USE_PREV;
        }
    }
}

static void* carve_top(uint32_t request) {
    MallocChunk* c = g_state.top_chunk_;
    const uint32_t rem = g_state.top_size_ - request;

    if (rem >= MALLOC_MIN_SIZE) {
        g_state.top_chunk_ = (MallocChunk*)((char*)c + request);
        g_state.top_size_ = rem;

        chunk_set_size_and_flags(c, request, CHUNK_IN_USE_PREV);

        return chunk_to_mem(c);
    }

    const uint32_t size = g_state.top_size_;

    g_state.top_chunk_ = NULL;
    g_state.top_size_ = 0u;

    chunk_set_size_and_flags(c, size, CHUNK_IN_USE_PREV);

    return chunk_to_mem(c);
}

static void* malloc_internal(size_t size) {
    if (unlikely(size == 0u)) {
        return NULL;
    }

    uint32_t request = (uint32_t)((size + 4u + 7u) & ~7u);

    if (request < MALLOC_MIN_SIZE) {
        request = MALLOC_MIN_SIZE;
    }

    const uint32_t idx = compute_bin_index(request);

    if (idx <= 30u) {
        MallocChunk* head = &g_state.bins_[idx];

        if (head->fd_ != head) {
            MallocChunk* c = head->fd_;
            bin_remove(c);

            c->size_ &= ~CHUNK_IS_FREE;

            MallocChunk* next = chunk_next(c);

            if (next != g_state.top_chunk_) {
                next->size_ |= CHUNK_IN_USE_PREV;
            }

            return chunk_to_mem(c);
        }
    }

    for (uint32_t i = idx; i < NUM_BINS; i++) {
        MallocChunk* head = &g_state.bins_[i];
        MallocChunk* c = head->fd_;

        while (c != head) {
            const uint32_t c_size = chunk_size(c);

            if (c_size >= request) {
                bin_remove(c);
                c->size_ &= ~CHUNK_IS_FREE;

                split_and_insert(c, c_size, request);

                return chunk_to_mem(c);
            }

            c = c->fd_;
        }
    }

    if (g_state.top_size_ < request) {
        if (unlikely(!extend_heap(request))) {
            return NULL;
        }
    }

    return carve_top(request);
}

static void free_internal(void* mem) {
    if (unlikely(!mem)) {
        return;
    }

    if (unlikely((uintptr_t)mem % MALLOC_ALIGNMENT != 0u)) {
        return;
    }

    MallocChunk* c = mem_to_chunk(mem);

    if (unlikely(chunk_is_free(c))) {
        return;
    }

    uint32_t size = chunk_size(c);

    if (!chunk_prev_in_use(c)) {
        const uint32_t prev_sz = c->prev_size_;
        MallocChunk* prev = (MallocChunk*)((char*)c - prev_sz);

        bin_remove(prev);

        c = prev;
        size += prev_sz;
    }

    MallocChunk* next = (MallocChunk*)((char*)c + size);

    if (next == g_state.top_chunk_) {
        g_state.top_chunk_ = c;
        g_state.top_size_ += size;

        if (g_state.top_size_ > MALLOC_TRIM_THRESHOLD) {
            uint32_t shrink = g_state.top_size_ - MALLOC_SYS_ALLOC_MIN;
            shrink &= ~4095u;

            if (shrink > 0u) {
                void* ret = sbrk(-(int)shrink);

                if ((int)(uintptr_t)ret != -1) {
                    g_state.top_size_ -= shrink;
                }
            }
        }

        return;
    }

    if (chunk_is_free(next)) {
        const uint32_t next_sz = chunk_size(next);

        bin_remove(next);
        size += next_sz;

        next = (MallocChunk*)((char*)c + size);
    }

    chunk_set_size_and_flags(c, size, CHUNK_IN_USE_PREV | CHUNK_IS_FREE);

    if (next == g_state.top_chunk_) {
        g_state.top_chunk_ = c;
        g_state.top_size_ += size;

        return;
    }

    next->size_ &= ~CHUNK_IN_USE_PREV;
    next->prev_size_ = size;

    bin_insert(c, size);
}

static void* realloc_internal(void* ptr, size_t size) {
    MallocChunk* c = mem_to_chunk(ptr);
    const uint32_t old_size = chunk_size(c);

    uint32_t request = (uint32_t)((size + 4u + 7u) & ~7u);

    if (request < MALLOC_MIN_SIZE) {
        request = MALLOC_MIN_SIZE;
    }

    if (old_size >= request) {
        split_and_insert(c, old_size, request);

        return ptr;
    }

    MallocChunk* next = chunk_next(c);

    if (next == g_state.top_chunk_) {
        const uint32_t needed = request - old_size;

        if (g_state.top_size_ < needed) {
            if (unlikely(!extend_heap(needed))) {
                goto slow_path;
            }

            next = chunk_next(c);

            if (unlikely(next != g_state.top_chunk_)) {
                goto slow_path;
            }
        }

        g_state.top_chunk_ = (MallocChunk*)((char*)next + needed);
        g_state.top_size_ -= needed;

        chunk_set_size_and_flags(c, request, c->size_ & CHUNK_FLAG_MASK);

        return ptr;
    }

    if (chunk_is_free(next)) {
        const uint32_t next_sz = chunk_size(next);

        if (old_size + next_sz >= request) {
            bin_remove(next);

            const uint32_t total = old_size + next_sz;
            c->size_ = total | (c->size_ & CHUNK_FLAG_MASK);

            split_and_insert(c, total, request);

            return ptr;
        }
    }

slow_path:
    void* new_ptr = malloc_internal(size);

    if (unlikely(!new_ptr)) {
        return NULL;
    }

    uint32_t copy_size = old_size - 4u;

    if (copy_size > size) {
        copy_size = (uint32_t)size;
    }

    memcpy(new_ptr, ptr, copy_size);
    free_internal(ptr);

    return new_ptr;
}

void* malloc(size_t size) {
    lock_heap();
    ensure_inited();

    void* res = malloc_internal(size);

    unlock_heap();
    return res;
}

void free(void* ptr) {
    if (unlikely(!ptr)) {
        return;
    }

    lock_heap();

    free_internal(ptr);

    unlock_heap();
}

void* calloc(size_t nelem, size_t elsize) {
    const size_t size = nelem * elsize;

    if (unlikely(nelem != 0u && size / nelem != elsize)) {
        return NULL;
    }

    lock_heap();
    ensure_inited();

    void* ptr = malloc_internal(size);

    if (likely(ptr != NULL)) {
        memset(ptr, 0, size);
    }

    unlock_heap();
    return ptr;
}

void* realloc(void* ptr, size_t size) {
    if (unlikely(!ptr)) {
        return malloc(size);
    }

    if (unlikely(size == 0u)) {
        free(ptr);
        return NULL;
    }

    lock_heap();
    ensure_inited();

    void* res = realloc_internal(ptr, size);

    unlock_heap();
    return res;
}