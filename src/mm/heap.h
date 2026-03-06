/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2025 Yula1234 */

#ifndef MM_HEAP_H
#define MM_HEAP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Kernel heap allocator.
 *
 * Small allocations are served from size-segregated caches (SLUB-like).
 * Larger requests are backed by whole pages via VMM.
 *
 * Pointers returned here are kernel virtual addresses.
 */

void heap_init(void);

/* Allocate `size` bytes. Returns nullptr on failure. */
void* kmalloc(size_t size);

/* Same as kmalloc(), but zero-fill the allocated object. */
void* kzalloc(size_t size);

/* Resize allocation. If `ptr` is nullptr, behaves like kmalloc(new_size). */
void* krealloc(void* ptr, size_t new_size);

/* Free a pointer previously returned by kmalloc/kzalloc/krealloc/kmalloc_*(). */
void kfree(void* ptr);

/*
 * Allocate memory with requested alignment.
 *
 * For align == PAGE_SIZE use kmalloc_a(), which returns a page-aligned block.
 */
void* kmalloc_aligned(size_t size, uint32_t align);

/* Allocate `size` bytes aligned to PAGE_SIZE (backed by whole pages). */
void* kmalloc_a(size_t size);

typedef struct kmem_cache kmem_cache_t;

/* Create a custom cache for fixed-size objects. Returns nullptr on failure. */
kmem_cache_t* kmem_cache_create(const char* name, size_t size, uint32_t align, uint32_t flags);

/* Allocate/free one object from a cache. */
void* kmem_cache_alloc(kmem_cache_t* cache);
void kmem_cache_free(kmem_cache_t* cache, void* obj);

/* Destroy a cache if it has no live objects. Returns 1 on success, 0 otherwise. */
int kmem_cache_destroy(kmem_cache_t* cache);

#ifdef __cplusplus
}
#endif

#endif
