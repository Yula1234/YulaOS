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
#define MALLOC_MMAP_THRESHOLD  (128u * 1024u)

#define CHUNK_IN_USE_PREV      1u
#define CHUNK_IS_FREE          2u
#define CHUNK_IS_MMAPPED       4u
#define CHUNK_FLAG_MASK        7u

#define NUM_BINS               64u

#define TCACHE_SLOTS           16u
#define TCACHE_MAX_BINS        32u
#define TCACHE_MAX_ENTRIES     64u
#define TCACHE_MAX_SIZE        256u

typedef struct malloc_chunk {
    uint32_t prev_size_;
    uint32_t size_;

    struct malloc_chunk* fd_;
    struct malloc_chunk* bk_;
} malloc_chunk_t;

typedef struct tcache_bin {
    malloc_chunk_t* head_;
    uint32_t count_;
} tcache_bin_t;

typedef struct tcache {
    tcache_bin_t bins_[TCACHE_MAX_BINS];
    uint32_t pid_;
} tcache_t;

typedef struct malloc_state {
    malloc_chunk_t bins_[NUM_BINS];

    malloc_chunk_t* top_chunk_;
    uint32_t top_size_;

    int initialized_;
} malloc_state_t;

#define ___inline __attribute__((always_inline)) static inline

static pthread_mutex_t g_malloc_lock = PTHREAD_MUTEX_INITIALIZER;

static malloc_state_t g_state;

static tcache_t g_tcaches[TCACHE_SLOTS];

static int g_zero_fd = -1;

___inline void lock_heap(void) {
    (void)pthread_mutex_lock(&g_malloc_lock);
}

___inline void unlock_heap(void) {
    (void)pthread_mutex_unlock(&g_malloc_lock);
}

___inline uint32_t chunk_size(const malloc_chunk_t* c) {
    return c->size_ & ~CHUNK_FLAG_MASK;
}

___inline int chunk_prev_in_use(const malloc_chunk_t* c) {
    return (c->size_ & CHUNK_IN_USE_PREV) != 0u;
}

___inline int chunk_is_free(const malloc_chunk_t* c) {
    return (c->size_ & CHUNK_IS_FREE) != 0u;
}

___inline malloc_chunk_t* chunk_next(const malloc_chunk_t* c) {
    return (malloc_chunk_t*)((char*)c + chunk_size(c));
}

___inline void chunk_set_size_and_flags(malloc_chunk_t* c, uint32_t size, uint32_t flags) {
    c->size_ = size | flags;
}

___inline void* chunk_to_mem(malloc_chunk_t* c) {
    return (void*)((char*)c + 8u);
}

___inline malloc_chunk_t* mem_to_chunk(void* mem) {
    return (malloc_chunk_t*)((char*)mem - 8u);
}

___inline uint32_t compute_bin_index(uint32_t size) {
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

___inline void bin_insert(malloc_chunk_t* chunk, uint32_t size) {
    const uint32_t idx = compute_bin_index(size);
    malloc_chunk_t* head = &g_state.bins_[idx];

    malloc_chunk_t* fwd = head->fd_;

    chunk->fd_ = fwd;
    chunk->bk_ = head;

    fwd->bk_ = chunk;
    head->fd_ = chunk;
}

___inline void bin_remove(malloc_chunk_t* chunk) {
    malloc_chunk_t* fwd = chunk->fd_;
    malloc_chunk_t* bck = chunk->bk_;

    if (unlikely(fwd->bk_ != chunk 
        || bck->fd_ != chunk)) {
        
        abort(); /* Safe Unlinking, Heap corruption detected. */
    }

    bck->fd_ = fwd;
    fwd->bk_ = bck;
}

___inline void ensure_inited(void) {
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

___inline tcache_t* get_tcache(void) {
    const uint32_t pid = (uint32_t)getpid();
    const uint32_t slot = pid & (TCACHE_SLOTS - 1u);

    tcache_t* t = &g_tcaches[slot];
    const uint32_t cached_pid = __atomic_load_n(&t->pid_, __ATOMIC_RELAXED);

    if (likely(cached_pid == pid)) {
        return t;
    }

    if (cached_pid == 0u) {
        uint32_t expected = 0u;
        const bool claimed = __atomic_compare_exchange_n(
            &t->pid_, &expected, pid, false,
            __ATOMIC_ACQ_REL, __ATOMIC_RELAXED
        );

        if (claimed) {
            return t;
        }
    }

    return NULL;
}

static void* mmap_alloc(uint32_t size) {
    int fd = __atomic_load_n(&g_zero_fd, __ATOMIC_ACQUIRE);

    if (unlikely(fd < 0)) {
        const int new_fd = open("/dev/zero", 0);

        if (unlikely(new_fd < 0)) {
            return NULL;
        }

        int expected = -1;
        const bool claimed = __atomic_compare_exchange_n(
            &g_zero_fd, &expected, new_fd, false,
            __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE
        );

        if (!claimed) {
            (void)close(new_fd);
        }

        fd = __atomic_load_n(&g_zero_fd, __ATOMIC_ACQUIRE);
    }

    const uint32_t alloc_size = size + 8u;

    void* ptr = mmap(fd, alloc_size, MAP_PRIVATE);

    if (unlikely(!ptr || ptr == (void*)-1)) {
        return NULL;
    }

    malloc_chunk_t* chunk = (malloc_chunk_t*)ptr;

    chunk->size_ = size | CHUNK_IN_USE_PREV | CHUNK_IS_MMAPPED;

    return (void*)((char*)chunk + 8u);
}

static void mmap_free(malloc_chunk_t* chunk) {
    const uint32_t size = chunk_size(chunk);
    const uint32_t alloc_size = size + 8u;

    (void)munmap((void*)chunk, alloc_size);
}

static void free_internal(malloc_chunk_t* c, uint32_t size);

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
                malloc_chunk_t* dummy = (malloc_chunk_t*)((char*)g_state.top_chunk_ + old_size - 8u);
                chunk_set_size_and_flags(dummy, 8u, CHUNK_IN_USE_PREV);

                malloc_chunk_t* old_top = g_state.top_chunk_;
                chunk_set_size_and_flags(old_top, old_size - 8u, CHUNK_IN_USE_PREV);

                free_internal(old_top, old_size - 8u);
            } else {
                malloc_chunk_t* dummy = g_state.top_chunk_;
                chunk_set_size_and_flags(dummy, old_size, CHUNK_IN_USE_PREV);
            }
        }

        g_state.top_chunk_ = (malloc_chunk_t*)p;
        g_state.top_size_ = request;
    }

    return 1;
}

static void split_and_insert(malloc_chunk_t* c, uint32_t total_size, uint32_t request_size) {
    const uint32_t rem = total_size - request_size;

    if (rem >= MALLOC_MIN_SIZE) {
        chunk_set_size_and_flags(c, request_size, c->size_ & CHUNK_FLAG_MASK);

        malloc_chunk_t* remainder = chunk_next(c);
        chunk_set_size_and_flags(remainder, rem, CHUNK_IN_USE_PREV | CHUNK_IS_FREE);

        malloc_chunk_t* next_after = chunk_next(remainder);

        if (next_after == g_state.top_chunk_) {
            g_state.top_chunk_ = remainder;
            g_state.top_size_ += rem;
        } else {
            next_after->size_ &= ~CHUNK_IN_USE_PREV;
            next_after->prev_size_ = rem;

            bin_insert(remainder, rem);
        }
    } else {
        malloc_chunk_t* next = chunk_next(c);

        if (next != g_state.top_chunk_) {
            next->size_ |= CHUNK_IN_USE_PREV;
        }
    }
}

static void* carve_top(uint32_t request) {
    malloc_chunk_t* c = g_state.top_chunk_;
    const uint32_t rem = g_state.top_size_ - request;

    if (rem >= MALLOC_MIN_SIZE) {
        g_state.top_chunk_ = (malloc_chunk_t*)((char*)c + request);
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

static void* malloc_internal(size_t request) {
    const uint32_t idx = compute_bin_index(request);

    if (idx <= 30u) {
        malloc_chunk_t* head = &g_state.bins_[idx];

        if (head->fd_ != head) {
            malloc_chunk_t* c = head->fd_;
            bin_remove(c);

            c->size_ &= ~CHUNK_IS_FREE;

            malloc_chunk_t* next = chunk_next(c);

            if (next != g_state.top_chunk_) {
                next->size_ |= CHUNK_IN_USE_PREV;
            }

            return chunk_to_mem(c);
        }
    }

    for (uint32_t i = idx; i < NUM_BINS; i++) {
        malloc_chunk_t* head = &g_state.bins_[i];
        malloc_chunk_t* c = head->fd_;

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

static void free_internal(malloc_chunk_t* c, uint32_t size) {
    if (!chunk_prev_in_use(c)) {
        const uint32_t prev_sz = c->prev_size_;
        malloc_chunk_t* prev = (malloc_chunk_t*)((char*)c - prev_sz);

        bin_remove(prev);

        c = prev;
        size += prev_sz;
    }

    malloc_chunk_t* next = (malloc_chunk_t*)((char*)c + size);

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

        next = (malloc_chunk_t*)((char*)c + size);
    }

    chunk_set_size_and_flags(c, size, CHUNK_IN_USE_PREV | CHUNK_IS_FREE);

    next->size_ &= ~CHUNK_IN_USE_PREV;
    next->prev_size_ = size;

    bin_insert(c, size);
}

static void* realloc_internal(malloc_chunk_t* c, uint32_t old_size, uint32_t request) {
    if (old_size >= request) {
        split_and_insert(c, old_size, request);

        return chunk_to_mem(c);
    }

    malloc_chunk_t* next = chunk_next(c);

    if (next == g_state.top_chunk_) {
        const uint32_t needed = request - old_size;

        if (g_state.top_size_ < needed) {
            if (unlikely(!extend_heap(needed))) {
                return NULL;
            }

            next = chunk_next(c);

            if (unlikely(next != g_state.top_chunk_)) {
                return NULL;
            }
        }

        g_state.top_chunk_ = (malloc_chunk_t*)((char*)next + needed);
        g_state.top_size_ -= needed;

        chunk_set_size_and_flags(c, request, c->size_ & CHUNK_FLAG_MASK);

        return chunk_to_mem(c);
    }

    if (chunk_is_free(next)) {
        const uint32_t next_sz = chunk_size(next);

        if (old_size + next_sz >= request) {
            bin_remove(next);

            const uint32_t total = old_size + next_sz;
            c->size_ = total | (c->size_ & CHUNK_FLAG_MASK);

            split_and_insert(c, total, request);

            return chunk_to_mem(c);
        }
    }

    return NULL;
}

void* malloc(size_t size) {
    if (unlikely(size == 0u)) {
        return NULL;
    }

    uint32_t request = (uint32_t)((size + 4u + 7u) & ~7u);

    if (request < MALLOC_MIN_SIZE) {
        request = MALLOC_MIN_SIZE;
    }

    if (unlikely(request >= MALLOC_MMAP_THRESHOLD)) {
        return mmap_alloc(request);
    }

    if (likely(request <= TCACHE_MAX_SIZE)) {
        tcache_t* tcache = get_tcache();

        if (likely(tcache != NULL)) {
            const uint32_t idx = (request - MALLOC_MIN_SIZE) >> 3u;
            tcache_bin_t* bin = &tcache->bins_[idx];

            if (bin->count_ > 0u) {
                malloc_chunk_t* c = bin->head_;
                
                bin->head_ = c->fd_;
                bin->count_--;

                return chunk_to_mem(c);
            }
        }
    }

    lock_heap();
    ensure_inited();

    void* res = malloc_internal(request);

    unlock_heap();

    return res;
}

void free(void* ptr) {
    if (unlikely(!ptr)) {
        return;
    }

    malloc_chunk_t* c = mem_to_chunk(ptr);

    if (unlikely((c->size_ & CHUNK_IS_MMAPPED) != 0u)) {
        mmap_free(c);

        return;
    }

    const uint32_t size = chunk_size(c);

    if (likely(size <= TCACHE_MAX_SIZE)) {
        tcache_t* tcache = get_tcache();

        if (likely(tcache != NULL)) {
            const uint32_t idx = (size - MALLOC_MIN_SIZE) >> 3u;
            tcache_bin_t* bin = &tcache->bins_[idx];

            if (bin->count_ < TCACHE_MAX_ENTRIES) {
                if (unlikely(bin->head_ == c)) {
                    abort(); /* Double free detected in fast path */
                }

                c->fd_ = bin->head_;
                
                bin->head_ = c;
                bin->count_++;

                return;
            }
        }
    }

    lock_heap();

    free_internal(c, size);

    unlock_heap();
}

void* calloc(size_t nelem, size_t elsize) {
    const size_t size = nelem * elsize;

    if (unlikely(nelem != 0u 
        && size / nelem != elsize)) {
        
        return NULL;
    }

    void* ptr = malloc(size);

    if (likely(ptr != NULL)) {
        memset(ptr, 0, size);
    }

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

    malloc_chunk_t* c = mem_to_chunk(ptr);

    if (unlikely((c->size_ & CHUNK_IS_MMAPPED) != 0u)) {
        const uint32_t old_size = chunk_size(c);
        const uint32_t copy_size = (size < old_size) ? (uint32_t)size : old_size;

        void* new_ptr = malloc(size);

        if (likely(new_ptr != NULL)) {
            memcpy(new_ptr, ptr, copy_size);
            free(ptr);
        }

        return new_ptr;
    }

    uint32_t request = (uint32_t)((size + 4u + 7u) & ~7u);

    if (request < MALLOC_MIN_SIZE) {
        request = MALLOC_MIN_SIZE;
    }

    const uint32_t old_size = chunk_size(c);

    lock_heap();
    ensure_inited();

    void* res = realloc_internal(c, old_size, request);

    unlock_heap();

    if (res != NULL) {
        return res;
    }

    void* new_ptr = malloc(size);

    if (likely(new_ptr != NULL)) {
        const uint32_t copy_size = (old_size - 4u < size) ? old_size - 4u : (uint32_t)size;
        
        memcpy(new_ptr, ptr, copy_size);
        free(ptr);
    }

    return new_ptr;
}