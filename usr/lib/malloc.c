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
#define MALLOC_MMAP_THRESHOLD  (128u * 1024u)

#define CHUNK_IN_USE_PREV      1u
#define CHUNK_IS_FREE          2u
#define CHUNK_IS_MMAPPED       4u
#define CHUNK_FLAG_MASK        7u

#define NUM_BINS               64u
#define NUM_ARENAS             4u

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
    struct tcache* self_;
    tcache_bin_t bins_[TCACHE_MAX_BINS];
} tcache_t;

typedef struct malloc_state {
    pthread_mutex_t lock_;
    uint32_t index_;

    malloc_chunk_t bins_[NUM_BINS];
    uint64_t binmap_;

    malloc_chunk_t* top_chunk_;
    uint32_t top_size_;

    int initialized_;
} malloc_state_t;

static malloc_state_t g_arenas[NUM_ARENAS];

static uint8_t g_page_to_arena[1048576];

static int g_zero_fd = -1;

static inline void set_tls_direct(void* tls_base) {
    (void)syscall(56, (int)(uintptr_t)tls_base, 0, 0);
}

static inline uint32_t chunk_size(const malloc_chunk_t* c) {
    return c->size_ & ~CHUNK_FLAG_MASK;
}

static inline int chunk_prev_in_use(const malloc_chunk_t* c) {
    return (c->size_ & CHUNK_IN_USE_PREV) != 0u;
}

static inline int chunk_is_free(const malloc_chunk_t* c) {
    return (c->size_ & CHUNK_IS_FREE) != 0u;
}

static inline malloc_chunk_t* chunk_next(const malloc_chunk_t* c) {
    return (malloc_chunk_t*)((char*)c + chunk_size(c));
}

static inline void chunk_set_size_and_flags(malloc_chunk_t* c, uint32_t size, uint32_t flags) {
    c->size_ = size | flags;
}

static inline void* chunk_to_mem(malloc_chunk_t* c) {
    return (void*)((char*)c + 8u);
}

static inline malloc_chunk_t* mem_to_chunk(void* mem) {
    return (malloc_chunk_t*)((char*)mem - 8u);
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

static void bin_insert(malloc_state_t* arena, malloc_chunk_t* chunk, uint32_t size) {
    const uint32_t idx = compute_bin_index(size);
    malloc_chunk_t* head = &arena->bins_[idx];

    malloc_chunk_t* fwd = head->fd_;

    chunk->fd_ = fwd;
    chunk->bk_ = head;

    fwd->bk_ = chunk;
    head->fd_ = chunk;

    arena->binmap_ |= (1ULL << idx);
}

static void bin_remove(malloc_state_t* arena, malloc_chunk_t* chunk) {
    malloc_chunk_t* fwd = chunk->fd_;
    malloc_chunk_t* bck = chunk->bk_;

    if (unlikely(fwd->bk_ != chunk 
        || bck->fd_ != chunk)) {
        
        __builtin_trap(); 
    }

    bck->fd_ = fwd;
    fwd->bk_ = bck;

    if (unlikely(fwd == bck)) {
        const uint32_t idx = compute_bin_index(chunk_size(chunk));
        arena->binmap_ &= ~(1ULL << idx);
    }
}

static void ensure_inited(malloc_state_t* arena) {
    if (likely(arena->initialized_)) {
        return;
    }

    arena->index_ = (uint32_t)(arena - g_arenas);

    for (uint32_t i = 0; i < NUM_BINS; i++) {
        arena->bins_[i].fd_ = &arena->bins_[i];
        arena->bins_[i].bk_ = &arena->bins_[i];
        arena->bins_[i].size_ = 0u;
    }

    arena->binmap_ = 0ull;

    arena->top_chunk_ = NULL;
    arena->top_size_ = 0u;

    arena->initialized_ = 1;
}

static malloc_state_t* acquire_arena(void) {
    const uint32_t pid = (uint32_t)getpid();
    const uint32_t preferred = pid & (NUM_ARENAS - 1u);

    if (pthread_mutex_trylock(&g_arenas[preferred].lock_) == 0) {
        return &g_arenas[preferred];
    }

    for (uint32_t i = 1; i < NUM_ARENAS; i++) {
        const uint32_t idx = (preferred + i) & (NUM_ARENAS - 1u);
        
        if (pthread_mutex_trylock(&g_arenas[idx].lock_) == 0) {
            return &g_arenas[idx];
        }
    }

    (void)pthread_mutex_lock(&g_arenas[preferred].lock_);
    
    return &g_arenas[preferred];
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

static inline tcache_t* get_tcache(void) {
    uint16_t fs_sel;
    
    __asm__ volatile ("mov %%fs, %0" : "=r"(fs_sel));

    if (unlikely(fs_sel == 0x23 || fs_sel == 0x2B)) {
        tcache_t* tcb = (tcache_t*)mmap_alloc(sizeof(tcache_t));
        
        if (unlikely(!tcb)) {
            return NULL;
        }
        
        for (uint32_t i = 0; i < TCACHE_MAX_BINS; i++) {
            tcb->bins_[i].head_ = NULL;
            tcb->bins_[i].count_ = 0u;
        }
        
        tcb->self_ = tcb;
        set_tls_direct(tcb);
        
        return tcb;
    }
    
    tcache_t* tcb;
    __asm__ volatile ("mov %%fs:0, %0" : "=r"(tcb));
    
    return tcb;
}

static void free_internal(malloc_state_t* arena, malloc_chunk_t* c, uint32_t size);

static int extend_heap(malloc_state_t* arena, uint32_t min_size) {
    uint32_t request = min_size + 8u;

    if (request < MALLOC_SYS_ALLOC_MIN) {
        request = MALLOC_SYS_ALLOC_MIN;
    }

    request = (request + 4095u) & ~4095u;

    int fd = __atomic_load_n(&g_zero_fd, __ATOMIC_ACQUIRE);

    if (unlikely(fd < 0)) {
        const int new_fd = open("/dev/zero", 0);

        if (unlikely(new_fd < 0)) {
            return 0;
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

    void* p = mmap(fd, request, MAP_PRIVATE);

    if (unlikely(!p || p == (void*)-1)) {
        return 0;
    }

    const uint32_t start_page = (uint32_t)(uintptr_t)p >> 12u;
    const uint32_t end_page = ((uint32_t)(uintptr_t)p + request - 1u) >> 12u;

    for (uint32_t i = start_page; i <= end_page; i++) {
        g_page_to_arena[i] = (uint8_t)arena->index_;
    }

    malloc_chunk_t* end_dummy = (malloc_chunk_t*)((char*)p + request - 8u);
    chunk_set_size_and_flags(end_dummy, 8u, CHUNK_IN_USE_PREV);

    if (arena->top_chunk_ != NULL && arena->top_size_ > 0u) {
        const uint32_t old_size = arena->top_size_;

        if (old_size >= MALLOC_MIN_SIZE + 8u) {
            malloc_chunk_t* dummy = (malloc_chunk_t*)((char*)arena->top_chunk_ + old_size - 8u);
            chunk_set_size_and_flags(dummy, 8u, CHUNK_IN_USE_PREV);

            malloc_chunk_t* old_top = arena->top_chunk_;
            chunk_set_size_and_flags(old_top, old_size - 8u, CHUNK_IN_USE_PREV);

            free_internal(arena, old_top, old_size - 8u);
        } else {
            malloc_chunk_t* dummy = arena->top_chunk_;
            chunk_set_size_and_flags(dummy, old_size, CHUNK_IN_USE_PREV);
        }
    }

    arena->top_chunk_ = (malloc_chunk_t*)p;
    arena->top_size_ = request - 8u;

    return 1;
}

static void split_and_insert(malloc_state_t* arena, malloc_chunk_t* c, uint32_t total_size, uint32_t request_size) {
    const uint32_t rem = total_size - request_size;

    if (rem >= MALLOC_MIN_SIZE) {
        chunk_set_size_and_flags(c, request_size, c->size_ & CHUNK_FLAG_MASK);

        malloc_chunk_t* remainder = chunk_next(c);
        chunk_set_size_and_flags(remainder, rem, CHUNK_IN_USE_PREV | CHUNK_IS_FREE);

        malloc_chunk_t* next_after = chunk_next(remainder);

        if (next_after == arena->top_chunk_) {
            arena->top_chunk_ = remainder;
            arena->top_size_ += rem;
        } else {
            next_after->size_ &= ~CHUNK_IN_USE_PREV;
            next_after->prev_size_ = rem;

            bin_insert(arena, remainder, rem);
        }
    } else {
        malloc_chunk_t* next = chunk_next(c);

        if (next != arena->top_chunk_) {
            next->size_ |= CHUNK_IN_USE_PREV;
        }
    }
}

static void* carve_top(malloc_state_t* arena, uint32_t request) {
    malloc_chunk_t* c = arena->top_chunk_;
    const uint32_t rem = arena->top_size_ - request;

    if (rem >= MALLOC_MIN_SIZE) {
        arena->top_chunk_ = (malloc_chunk_t*)((char*)c + request);
        arena->top_size_ = rem;

        chunk_set_size_and_flags(c, request, CHUNK_IN_USE_PREV);

        return chunk_to_mem(c);
    }

    const uint32_t size = arena->top_size_;

    arena->top_chunk_ = NULL;
    arena->top_size_ = 0u;

    chunk_set_size_and_flags(c, size, CHUNK_IN_USE_PREV);

    return chunk_to_mem(c);
}

static void* malloc_internal(malloc_state_t* arena, uint32_t request) {
    const uint32_t idx = compute_bin_index(request);
    uint64_t available_map = arena->binmap_ & (~0ULL << idx);

    while (available_map != 0) {
        const uint32_t i = (uint32_t)__builtin_ctzll(available_map);
        
        malloc_chunk_t* head = &arena->bins_[i];
        malloc_chunk_t* c = head->fd_;

        while (c != head) {
            const uint32_t c_size = chunk_size(c);

            if (c_size >= request) {
                bin_remove(arena, c);
                c->size_ &= ~CHUNK_IS_FREE;

                split_and_insert(arena, c, c_size, request);

                return chunk_to_mem(c);
            }

            c = c->fd_;
        }

        available_map &= ~(1ULL << i);
    }

    if (arena->top_size_ < request) {
        if (unlikely(!extend_heap(arena, request))) {
            return NULL;
        }
    }

    return carve_top(arena, request);
}

static void free_internal(malloc_state_t* arena, malloc_chunk_t* c, uint32_t size) {
    if (!chunk_prev_in_use(c)) {
        const uint32_t prev_sz = c->prev_size_;
        malloc_chunk_t* prev = (malloc_chunk_t*)((char*)c - prev_sz);

        bin_remove(arena, prev);

        c = prev;
        size += prev_sz;
    }

    malloc_chunk_t* next = (malloc_chunk_t*)((char*)c + size);

    if (next == arena->top_chunk_) {
        arena->top_chunk_ = c;
        arena->top_size_ += size;
        
        return;
    }

    if (chunk_is_free(next)) {
        const uint32_t next_sz = chunk_size(next);

        bin_remove(arena, next);
        size += next_sz;

        next = (malloc_chunk_t*)((char*)c + size);
    }

    chunk_set_size_and_flags(c, size, CHUNK_IN_USE_PREV | CHUNK_IS_FREE);

    next->size_ &= ~CHUNK_IN_USE_PREV;
    next->prev_size_ = size;

    bin_insert(arena, c, size);
}

static void* realloc_internal(malloc_state_t* arena, malloc_chunk_t* c, uint32_t old_size, uint32_t request) {
    if (old_size >= request) {
        split_and_insert(arena, c, old_size, request);

        return chunk_to_mem(c);
    }

    malloc_chunk_t* next = chunk_next(c);

    if (next == arena->top_chunk_) {
        const uint32_t needed = request - old_size;

        if (arena->top_size_ < needed) {
            if (unlikely(!extend_heap(arena, needed))) {
                return NULL;
            }

            next = chunk_next(c);

            if (unlikely(next != arena->top_chunk_)) {
                return NULL;
            }
        }

        arena->top_chunk_ = (malloc_chunk_t*)((char*)next + needed);
        arena->top_size_ -= needed;

        chunk_set_size_and_flags(c, request, c->size_ & CHUNK_FLAG_MASK);

        return chunk_to_mem(c);
    }

    if (chunk_is_free(next)) {
        const uint32_t next_sz = chunk_size(next);

        if (old_size + next_sz >= request) {
            bin_remove(arena, next);

            const uint32_t total = old_size + next_sz;
            c->size_ = total | (c->size_ & CHUNK_FLAG_MASK);

            split_and_insert(arena, c, total, request);

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

    malloc_state_t* arena = acquire_arena();
    
    ensure_inited(arena);

    void* res = malloc_internal(arena, request);

    (void)pthread_mutex_unlock(&arena->lock_);

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
                    __builtin_trap(); 
                }

                c->fd_ = bin->head_;
                
                bin->head_ = c;
                bin->count_++;

                return;
            }
        }
    }

    const uint32_t page_idx = (uint32_t)(uintptr_t)ptr >> 12u;
    const uint8_t arena_idx = g_page_to_arena[page_idx];

    malloc_state_t* arena = &g_arenas[arena_idx];

    (void)pthread_mutex_lock(&arena->lock_);
    
    free_internal(arena, c, size);
    
    (void)pthread_mutex_unlock(&arena->lock_);
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

    const uint32_t page_idx = (uint32_t)(uintptr_t)ptr >> 12u;
    const uint8_t arena_idx = g_page_to_arena[page_idx];

    malloc_state_t* arena = &g_arenas[arena_idx];

    (void)pthread_mutex_lock(&arena->lock_);
    
    ensure_inited(arena);

    void* res = realloc_internal(arena, c, old_size, request);

    (void)pthread_mutex_unlock(&arena->lock_);

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